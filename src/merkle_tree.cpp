#include "merkle_tree.h"

#include "content_store.h"
#include "performance_profile.h"

#include <algorithm>
#include <array>
#include <utility>

namespace aowis::db::merkle_tree
{
namespace
{
constexpr int key_nibbles = 32;
constexpr int branches = 16;
constexpr int hash_bytes = 32;
constexpr int header_bytes = 22; // "AOWIS-MERKLE-RADIX-V1" + NUL
const QByteArray header("AOWIS-MERKLE-RADIX-V1\0", header_bytes);

struct Node
{
    QByteArray prefix; // unpacked nibbles (0..15), local to its parent edge
    std::optional<QByteArray> value;
    std::array<QByteArray, branches> children;
};

void setError(QString *error, const QString &message)
{
    if (error != nullptr)
        *error = message;
}

QByteArray unpackKey(const QUuid &key)
{
    QByteArray nibbles;
    nibbles.reserve(key_nibbles);
    const QByteArray bytes = key.toRfc4122();
    for (char raw : bytes)
    {
        const quint8 byte = static_cast<quint8>(raw);
        nibbles.append(static_cast<char>(byte >> 4));
        nibbles.append(static_cast<char>(byte & 0x0f));
    }
    return nibbles;
}

QUuid packKey(const QByteArray &nibbles)
{
    QByteArray bytes;
    bytes.reserve(16);
    for (int i = 0; i < key_nibbles; i += 2)
    {
        const quint8 high = static_cast<quint8>(nibbles.at(i));
        const quint8 low = static_cast<quint8>(nibbles.at(i + 1));
        bytes.append(static_cast<char>((high << 4) | low));
    }
    return QUuid::fromRfc4122(bytes);
}

int childrenCount(const Node &node)
{
    int count = 0;
    for (const QByteArray &child : node.children)
        count += !child.isEmpty() ? 1 : 0;
    return count;
}

int singleChildIndex(const Node &node)
{
    for (int i = 0; i < branches; ++i)
    {
        if (!node.children[i].isEmpty())
            return i;
    }
    return -1;
}

// Canonical representation: magic, prefix-nibble-count (1 byte), packed
// prefix (zero padding in low nibble), leaf-value flag (1 byte), optional
// blob digest (32 bytes), child bitmap (2 bytes BE), child hashes in nibble
// order. There are no offsets, host-endian fields or QByteArray wrappers.
QByteArray encode(const Node &node)
{
    QByteArray bytes = header;
    bytes.append(static_cast<char>(node.prefix.size()));
    for (int i = 0; i < node.prefix.size(); i += 2)
    {
        const quint8 hi = static_cast<quint8>(node.prefix.at(i));
        const quint8 lo = i + 1 < node.prefix.size()
                                ? static_cast<quint8>(node.prefix.at(i + 1)) : 0;
        bytes.append(static_cast<char>((hi << 4) | lo));
    }
    bytes.append(node.value ? '\x01' : '\x00');
    if (node.value)
        bytes.append(*node.value);

    quint16 mask = 0;
    for (int i = 0; i < branches; ++i)
    {
        if (!node.children[i].isEmpty())
            mask = static_cast<quint16>(mask | (1U << i));
    }
    bytes.append(static_cast<char>(mask >> 8));
    bytes.append(static_cast<char>(mask & 0xff));
    for (const QByteArray &child : node.children)
    {
        if (!child.isEmpty())
            bytes.append(child);
    }
    return bytes;
}

bool validShape(const Node &node, int depth, bool is_root, QString *error)
{
    if (depth < 0 || depth > key_nibbles ||
        node.prefix.size() > key_nibbles - depth)
    {
        setError(error, QStringLiteral("Merkle tree prefix exceeds UUID key length"));
        return false;
    }
    for (char nibble : node.prefix)
    {
        if (static_cast<quint8>(nibble) >= branches)
        {
            setError(error, QStringLiteral("Invalid Merkle prefix nibble"));
            return false;
        }
    }
    const int count = childrenCount(node);
    for (const QByteArray &child : node.children)
    {
        if (!child.isEmpty() && child.size() != hash_bytes)
        {
            setError(error, QStringLiteral("Invalid Merkle child digest length"));
            return false;
        }
    }
    if (depth + node.prefix.size() == key_nibbles)
    {
        if (!node.value || node.value->size() != hash_bytes || count != 0)
        {
            setError(error, QStringLiteral("Malformed Merkle leaf"));
            return false;
        }
    }
    else if (node.value || (count < 2 && !(is_root && depth == 0 &&
                                          node.prefix.isEmpty() && count == 0)))
    {
        setError(error, QStringLiteral("Noncanonical Merkle branch or empty node"));
        return false;
    }
    return true;
}

std::optional<Node> decode(const QByteArray &bytes, int depth, bool is_root, QString *error)
{
    if (bytes.size() < header_bytes + 4 || !bytes.startsWith(header))
    {
        setError(error, QStringLiteral("Invalid Merkle node magic or length"));
        return std::nullopt;
    }
    int pos = header_bytes;
    const int nibble_count = static_cast<quint8>(bytes.at(pos++));
    const int packed_size = (nibble_count + 1) / 2;
    if (nibble_count > key_nibbles || bytes.size() < pos + packed_size + 3)
    {
        setError(error, QStringLiteral("Invalid Merkle prefix encoding"));
        return std::nullopt;
    }
    Node node;
    node.prefix.reserve(nibble_count);
    for (int i = 0; i < packed_size; ++i)
    {
        const quint8 byte = static_cast<quint8>(bytes.at(pos++));
        node.prefix.append(static_cast<char>(byte >> 4));
        if (node.prefix.size() < nibble_count)
            node.prefix.append(static_cast<char>(byte & 0x0f));
        else if ((byte & 0x0f) != 0)
        {
            setError(error, QStringLiteral("Noncanonical Merkle prefix padding"));
            return std::nullopt;
        }
    }
    const quint8 has_value = static_cast<quint8>(bytes.at(pos++));
    if (has_value > 1 || bytes.size() < pos + (has_value ? hash_bytes : 0) + 2)
    {
        setError(error, QStringLiteral("Invalid Merkle value encoding"));
        return std::nullopt;
    }
    if (has_value)
    {
        node.value = bytes.mid(pos, hash_bytes);
        pos += hash_bytes;
    }
    const quint16 bitmap = static_cast<quint16>(
        (static_cast<quint8>(bytes.at(pos)) << 8) |
        static_cast<quint8>(bytes.at(pos + 1)));
    pos += 2;
    for (int i = 0; i < branches; ++i)
    {
        if ((bitmap & (1U << i)) != 0)
        {
            if (bytes.size() - pos < hash_bytes)
            {
                setError(error, QStringLiteral("Truncated Merkle child digest"));
                return std::nullopt;
            }
            node.children[i] = bytes.mid(pos, hash_bytes);
            pos += hash_bytes;
        }
    }
    if (pos != bytes.size() || !validShape(node, depth, is_root, error))
    {
        if (pos != bytes.size())
            setError(error, QStringLiteral("Trailing bytes in Merkle node"));
        return std::nullopt;
    }
    return node;
}

std::optional<Node> read(const QSqlDatabase &database, const QByteArray &digest,
                         int depth, bool is_root, QString *error)
{
    const std::optional<QByteArray> bytes = content_store::get(
        database, content_store::Kind::Tree, digest, error);
    if (!bytes)
        return std::nullopt;
    return decode(*bytes, depth, is_root, error);
}

std::optional<QByteArray> save(const QSqlDatabase &database, const Node &node,
                                int depth, bool is_root, QString *error)
{
    if (!validShape(node, depth, is_root, error))
        return std::nullopt;
    return content_store::put(database, content_store::Kind::Tree, encode(node), error);
}

Node leaf(const QByteArray &key, int depth, const QByteArray &hash)
{
    Node node;
    node.prefix = key.mid(depth);
    node.value = hash;
    return node;
}

int commonPrefix(const QByteArray &key, int depth, const QByteArray &prefix)
{
    int n = 0;
    while (n < prefix.size() && key.at(depth + n) == prefix.at(n))
        ++n;
    return n;
}

// null QByteArray is an internal empty-subtree marker, never published as a
// root. A nonempty root is always the digest of an immutable tree node.
std::optional<QByteArray> changeSubtree(const QSqlDatabase &database,
                                        const QByteArray &current_hash,
                                        const QByteArray &key, int depth,
                                        const std::optional<QByteArray> &new_value,
                                        QString *error)
{
    if (current_hash.isEmpty())
    {
        if (!new_value)
            return QByteArray();
        return save(database, leaf(key, depth, *new_value), depth, depth == 0, error);
    }
    const std::optional<Node> current = read(database, current_hash, depth, depth == 0, error);
    if (!current)
        return std::nullopt;
    const int common = commonPrefix(key, depth, current->prefix);
    if (common < current->prefix.size())
    {
        if (!new_value)
            return current_hash;
        // Split an existing compressed path into two immutable child nodes.
        const int fork_depth = depth + common;
        const int old_edge = static_cast<quint8>(current->prefix.at(common));
        const int new_edge = static_cast<quint8>(key.at(fork_depth));
        Node old_suffix = *current;
        old_suffix.prefix = current->prefix.mid(common + 1);
        const std::optional<QByteArray> old_hash = save(
            database, old_suffix, fork_depth + 1, false, error);
        if (!old_hash)
            return std::nullopt;
        const std::optional<QByteArray> new_hash = save(
            database, leaf(key, fork_depth + 1, *new_value), fork_depth + 1, false, error);
        if (!new_hash)
            return std::nullopt;
        Node fork;
        fork.prefix = current->prefix.left(common);
        fork.children[old_edge] = *old_hash;
        fork.children[new_edge] = *new_hash;
        return save(database, fork, depth, depth == 0, error);
    }
    const int next_depth = depth + current->prefix.size();
    if (next_depth == key_nibbles)
    {
        if (current->value == new_value)
            return current_hash;
        if (!new_value)
            return QByteArray();
        Node replacement = *current;
        replacement.value = *new_value;
        return save(database, replacement, depth, depth == 0, error);
    }
    if (childrenCount(*current) == 0) // only the canonical empty root
    {
        if (!new_value)
            return current_hash;
        return save(database, leaf(key, depth, *new_value), depth, depth == 0, error);
    }
    const int edge = static_cast<quint8>(key.at(next_depth));
    const std::optional<QByteArray> updated = changeSubtree(
        database, current->children[edge], key, next_depth + 1, new_value, error);
    if (!updated)
        return std::nullopt;
    if (*updated == current->children[edge])
        return current_hash;
    Node replacement = *current;
    replacement.children[edge] = *updated;
    const int count = childrenCount(replacement);
    if (count == 0)
        return QByteArray();
    if (count == 1)
    {
        // Compress a unary path after deletion instead of retaining chains.
        const int only_edge = singleChildIndex(replacement);
        const std::optional<Node> child = read(
            database, replacement.children[only_edge], next_depth + 1, false, error);
        if (!child)
            return std::nullopt;
        Node compact = *child;
        compact.prefix = replacement.prefix + QByteArray(1, static_cast<char>(only_edge)) + child->prefix;
        return save(database, compact, depth, depth == 0, error);
    }
    return save(database, replacement, depth, depth == 0, error);
}

struct BuildEntry
{
    QByteArray key; // 32 unpacked UUID nibbles
    QByteArray value;
};

// Construct a canonical path-compressed subtree bottom-up, without SQL
// lookups for its intermediate roots. Entries are sorted by their full UUID.
std::optional<QByteArray> buildRange(const QVector<BuildEntry> &items,
                                     qsizetype start, qsizetype end, int depth,
                                     QVector<content_store::PreparedContent> &nodes,
                                     QString *error)
{
    if (start >= end)
        return std::nullopt;
    Node node;
    if (end - start == 1)
    {
        node = leaf(items.at(start).key, depth, items.at(start).value);
    }
    else
    {
        const QByteArray &first = items.at(start).key;
        const QByteArray &last = items.at(end - 1).key;
        int common = 0;
        while (depth + common < key_nibbles &&
               first.at(depth + common) == last.at(depth + common))
            ++common;
        if (depth + common >= key_nibbles)
        {
            setError(error, QStringLiteral("Duplicate UUID in Merkle bulk build"));
            return std::nullopt;
        }
        node.prefix = first.mid(depth, common);
        const int fork_depth = depth + common;
        qsizetype cursor = start;
        while (cursor < end)
        {
            const int edge = static_cast<quint8>(items.at(cursor).key.at(fork_depth));
            qsizetype next = cursor + 1;
            while (next < end &&
                   static_cast<quint8>(items.at(next).key.at(fork_depth)) == edge)
                ++next;
            const std::optional<QByteArray> child = buildRange(
                items, cursor, next, fork_depth + 1, nodes, error);
            if (!child)
                return std::nullopt;
            node.children[edge] = *child;
            cursor = next;
        }
    }
    if (!validShape(node, depth, depth == 0, error))
        return std::nullopt;
    content_store::PreparedContent prepared(content_store::Kind::Tree, encode(node));
    const QByteArray hash = prepared.sha256();
    nodes.append(std::move(prepared));
    return hash;
}

// Breadth-first batched traversal: at each level, fetch all referenced
// immutable tree nodes in bounded SQL batches. This avoids a separate remote
// PostgreSQL query for every Merkle node in a full snapshot enumeration.
// The path and root status are validated independently for every reference.
bool scanBatched(const QSqlDatabase &database, const QByteArray &root,
                 QVector<Entry> &out, QHash<QByteArray, QByteArray> *verified_nodes,
                 QString *error)
{
    struct Pending
    {
        QByteArray digest;
        QByteArray leading;
        bool is_root = false;
    };
    QVector<Pending> pending{{root, QByteArray(), true}};
    while (!pending.isEmpty())
    {
        QVector<QByteArray> missing;
        missing.reserve(pending.size());
        for (const Pending &item : pending)
        {
            if (verified_nodes == nullptr || !verified_nodes->contains(item.digest))
                missing.append(item.digest);
        }
        const std::optional<QHash<QByteArray, QByteArray>> fetched =
            content_store::getMany(database, content_store::Kind::Tree, missing, error);
        if (!fetched)
            return false;
        if (verified_nodes != nullptr)
        {
            // Populate only from integrity-checked content_store results.
            for (QHash<QByteArray, QByteArray>::const_iterator it = fetched->cbegin();
                 it != fetched->cend(); ++it)
                verified_nodes->insert(it.key(), it.value());
        }

        QVector<Pending> children;
        for (const Pending &item : pending)
        {
            const QByteArray bytes = verified_nodes != nullptr
                ? verified_nodes->value(item.digest) : fetched->value(item.digest);
            const std::optional<Node> node = decode(
                bytes, item.leading.size(), item.is_root, error);
            if (!node)
                return false;
            const QByteArray path = item.leading + node->prefix;
            if (node->value)
            {
                out.append(Entry{packKey(path), *node->value});
                continue;
            }
            for (int edge = 0; edge < branches; ++edge)
            {
                if (!node->children[edge].isEmpty())
                {
                    children.append(Pending{node->children[edge],
                                            path + QByteArray(1, static_cast<char>(edge)), false});
                }
            }
        }
        pending = std::move(children);
    }
    std::sort(out.begin(), out.end(), [](const Entry &left, const Entry &right) {
        return left.key.toRfc4122() < right.key.toRfc4122();
    });
    for (qsizetype index = 1; index < out.size(); ++index)
    {
        if (out.at(index - 1).key == out.at(index).key)
        {
            setError(error, QStringLiteral("Duplicate UUID in Merkle tree"));
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<QByteArray> emptyRoot(const QSqlDatabase &database, QString *error)
{
    if (error != nullptr)
        error->clear();
    return save(database, Node{}, 0, true, error);
}

std::optional<QByteArray> lookup(const QSqlDatabase &database, const QByteArray &root,
                                 const QUuid &key, QString *error)
{
    if (error != nullptr)
        error->clear();
    const QByteArray nibbles = unpackKey(key);
    QByteArray current_hash = root;
    int depth = 0;
    while (true)
    {
        const std::optional<Node> node = read(database, current_hash, depth, depth == 0, error);
        if (!node)
            return std::nullopt;
        if (commonPrefix(nibbles, depth, node->prefix) != node->prefix.size())
            return std::nullopt;
        depth += node->prefix.size();
        if (depth == key_nibbles)
            return node->value;
        if (childrenCount(*node) == 0)
            return std::nullopt;
        const int edge = static_cast<quint8>(nibbles.at(depth++));
        if (node->children[edge].isEmpty())
            return std::nullopt;
        current_hash = node->children[edge];
    }
}

std::optional<QHash<QUuid, QByteArray>> lookupMany(
    const QSqlDatabase &database, const QByteArray &root,
    const QSet<QUuid> &requested, QString *error)
{
    if (error != nullptr)
        error->clear();
    QHash<QUuid, QByteArray> result;
    if (requested.isEmpty())
        return result;

    struct Key
    {
        QUuid uuid;
        QByteArray nibbles;
    };
    QVector<Key> keys;
    keys.reserve(requested.size());
    for (const QUuid &uuid : requested)
        keys.append(Key{uuid, unpackKey(uuid)});
    std::sort(keys.begin(), keys.end(), [](const Key &left, const Key &right) {
        return left.nibbles < right.nibbles;
    });

    // Each pending subtree owns a consecutive range of sorted requested UUIDs.
    // A shared prefix is fetched and SHA-256-verified only once per level,
    // rather than independently for each UUID. No unrelated subtree is read.
    struct Pending
    {
        QByteArray digest;
        qsizetype begin;
        qsizetype end;
        int depth;
    };
    QVector<Pending> pending{{root, 0, keys.size(), 0}};
    while (!pending.isEmpty())
    {
        QVector<QByteArray> hashes;
        hashes.reserve(pending.size());
        for (const Pending &item : pending)
            hashes.append(item.digest);
        const std::optional<QHash<QByteArray, QByteArray>> payloads =
            content_store::getMany(database, content_store::Kind::Tree, hashes, error);
        if (!payloads)
            return std::nullopt;

        QVector<Pending> next;
        for (const Pending &item : pending)
        {
            const std::optional<Node> node = decode(
                payloads->value(item.digest), item.depth, item.depth == 0, error);
            if (!node)
                return std::nullopt;
            const int after_prefix = item.depth + node->prefix.size();
            if (node->value)
            {
                // A compressed leaf represents exactly one UUID. Other
                // requested keys in the same parent range are simply absent.
                for (qsizetype i = item.begin; i < item.end; ++i)
                {
                    if (commonPrefix(keys.at(i).nibbles, item.depth, node->prefix)
                        == node->prefix.size())
                        result.insert(keys.at(i).uuid, *node->value);
                }
                continue;
            }
            qsizetype cursor = item.begin;
            while (cursor < item.end)
            {
                if (commonPrefix(keys.at(cursor).nibbles, item.depth, node->prefix)
                    != node->prefix.size())
                {
                    ++cursor;
                    continue;
                }
                const int edge = static_cast<quint8>(
                    keys.at(cursor).nibbles.at(after_prefix));
                qsizetype end = cursor + 1;
                while (end < item.end &&
                       commonPrefix(keys.at(end).nibbles, item.depth, node->prefix)
                           == node->prefix.size() &&
                       static_cast<quint8>(keys.at(end).nibbles.at(after_prefix)) == edge)
                    ++end;
                if (!node->children[edge].isEmpty())
                    next.append(Pending{node->children[edge], cursor, end, after_prefix + 1});
                cursor = end;
            }
        }
        pending = std::move(next);
    }
    return result;
}

std::optional<QByteArray> update(const QSqlDatabase &database, const QByteArray &root,
                                 const Change &change, QString *error)
{
    if (error != nullptr)
        error->clear();
    if (change.blob_sha256)
    {
        if (change.blob_sha256->size() != hash_bytes ||
            !content_store::get(database, content_store::Kind::Blob,
                                *change.blob_sha256, error))
        {
            if (error != nullptr && error->isEmpty())
                *error = QStringLiteral("Merkle value must reference an existing content blob");
            return std::nullopt;
        }
    }
    const QByteArray key = unpackKey(change.key);
    const std::optional<QByteArray> updated = changeSubtree(
        database, root, key, 0, change.blob_sha256, error);
    if (!updated)
        return std::nullopt;
    if (!updated->isEmpty())
        return updated;
    return save(database, Node{}, 0, true, error);
}

std::optional<QByteArray> apply(const QSqlDatabase &database, const QByteArray &root,
                                const QVector<Change> &changes, QString *error)
{
    if (error != nullptr)
        error->clear();
    QVector<Change> sorted = changes;
    std::sort(sorted.begin(), sorted.end(), [](const Change &left, const Change &right) {
        return left.key.toRfc4122() < right.key.toRfc4122();
    });
    for (int i = 1; i < sorted.size(); ++i)
    {
        if (sorted.at(i - 1).key == sorted.at(i).key)
        {
            setError(error, QStringLiteral("Duplicate UUID key in Merkle batch"));
            return std::nullopt;
        }
    }
    QByteArray current = root;
    for (const Change &change : sorted)
    {
        const std::optional<QByteArray> next = update(database, current, change, error);
        if (!next)
            return std::nullopt;
        current = *next;
    }
    // Even empty batches must verify the supplied root is a valid tree.
    if (sorted.isEmpty() && !read(database, root, 0, true, error))
        return std::nullopt;
    return current;
}

static std::optional<QByteArray> buildImpl(const QSqlDatabase &database,
                                           const QVector<Change> &contents,
                                           bool blobs_already_verified,
                                           QString *error)
{
    performance_profile::PhaseTimer phases("merkle_bulk_build");
    if (error != nullptr)
        error->clear();
    if (contents.isEmpty())
        return emptyRoot(database, error);

    QVector<BuildEntry> entries;
    entries.reserve(contents.size());
    QVector<QByteArray> leaf_hashes;
    leaf_hashes.reserve(contents.size());
    for (const Change &change : contents)
    {
        if (!change.blob_sha256 || change.blob_sha256->size() != hash_bytes)
        {
            setError(error, QStringLiteral("Bulk Merkle build requires existing blob digests"));
            return std::nullopt;
        }
        entries.append(BuildEntry{unpackKey(change.key), *change.blob_sha256});
        leaf_hashes.append(*change.blob_sha256);
    }
    std::sort(entries.begin(), entries.end(), [](const BuildEntry &left, const BuildEntry &right) {
        return left.key < right.key;
    });
    for (qsizetype i = 1; i < entries.size(); ++i)
    {
        if (entries.at(i - 1).key == entries.at(i).key)
        {
            setError(error, QStringLiteral("Duplicate UUID in Merkle bulk build"));
            return std::nullopt;
        }
    }
    phases.mark("sort_and_validate_keys", entries.size());
    // The first-seal caller obtained these hashes from putMany() in this
    // transaction. A general build() still independently validates input
    // hashes because its caller may supply arbitrary references.
    if (!blobs_already_verified &&
        !content_store::getMany(database, content_store::Kind::Blob, leaf_hashes, error))
        return std::nullopt;
    phases.mark("verify_blob_references", blobs_already_verified ? 0 : leaf_hashes.size());

    QVector<content_store::PreparedContent> nodes;
    nodes.reserve(entries.size() * 2);
    const std::optional<QByteArray> root = buildRange(
        entries, 0, entries.size(), 0, nodes, error);
    if (!root)
        return std::nullopt;
    phases.mark("construct_hash_nodes", nodes.size());
    if (!content_store::putManyPrepared(database, content_store::Kind::Tree, nodes, error))
        return std::nullopt;
    phases.mark("insert_tree_nodes", nodes.size());
    return root;
}

std::optional<QByteArray> build(const QSqlDatabase &database,
                                const QVector<Change> &contents, QString *error)
{
    return buildImpl(database, contents, false, error);
}

std::optional<QByteArray> buildFromInsertedBlobs(
    const QSqlDatabase &database, const QVector<Change> &contents, QString *error)
{
    return buildImpl(database, contents, true, error);
}

std::optional<QVector<Entry>> entries(const QSqlDatabase &database,
                                     const QByteArray &root, QString *error)
{
    if (error != nullptr)
        error->clear();
    QVector<Entry> result;
    if (!scanBatched(database, root, result, nullptr, error))
        return std::nullopt;
    return result;
}

std::optional<QVector<Entry>> entriesCached(const QSqlDatabase &database,
    const QByteArray &root, QHash<QByteArray, QByteArray> &verified_nodes,
    QString *error)
{
    if (error != nullptr)
        error->clear();
    QVector<Entry> result;
    if (!scanBatched(database, root, result, &verified_nodes, error))
        return std::nullopt;
    return result;
}

} // namespace aowis::db::merkle_tree
