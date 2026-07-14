#include <QCoreApplication>
#include <QDebug>
#include <QSqlDatabase>

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);

#ifdef AOWIS_SERVER_DB_GUI
    qDebug() << "Testing GUI database target";
#endif

#ifdef AOWIS_SERVER_DB_API
    qDebug() << "Testing API database target";
#endif

    qDebug() << "Available SQL drivers:" << QSqlDatabase::drivers();

    return 0;
}
