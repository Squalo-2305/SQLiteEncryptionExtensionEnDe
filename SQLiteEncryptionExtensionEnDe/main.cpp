#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <iostream>
#include <string>
#include <vector>

namespace
{
    struct QString
    {
        void* opaque[5]{};
    };

    struct SqlObject
    {
        void* opaque[8]{};
    };

    struct CoreApplicationObject
    {
        void* opaque[64]{};
    };

    using QStringFromStdStringFn = QString* (__fastcall*)(
        QString*, const std::string*);
    using QStringDtor = void (__fastcall*)(QString*);
    using AddLibraryPathFn = void (__fastcall*)(const QString*);
    using CoreApplicationCtor = CoreApplicationObject* (__fastcall*)(
        CoreApplicationObject*, int*, char**, int);
    using CoreApplicationDtor = void (__fastcall*)(CoreApplicationObject*);
    using AddDatabaseFn = void (__fastcall*)(
        SqlObject*, const QString*, const QString*);
    using SetDatabaseNameFn = void (__fastcall*)(SqlObject*, const QString*);
    using DatabaseOpenFn = bool (__fastcall*)(SqlObject*);
    using DatabaseCloseFn = void (__fastcall*)(SqlObject*);
    using QueryCtor = SqlObject* (__fastcall*)(
        SqlObject*, const QString*, const SqlObject*);
    using QueryExecFn = bool (__fastcall*)(SqlObject*, const QString*);
    using QueryFinishFn = void (__fastcall*)(SqlObject*);
    using SqlObjectDtor = void (__fastcall*)(SqlObject*);

    template <typename Function>
    Function loadFunction(HMODULE module, const char* name)
    {
        return reinterpret_cast<Function>(GetProcAddress(module, name));
    }

    std::wstring executableDirectory()
    {
        std::vector<wchar_t> path(32768);
        const DWORD length = GetModuleFileNameW(
            nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length == path.size())
        {
            return {};
        }

        const std::wstring executable(path.data(), length);
        const size_t separator = executable.find_last_of(L"\\/");
        return separator == std::wstring::npos
            ? std::wstring{}
            : executable.substr(0, separator + 1);
    }

    std::string toUtf8(const std::wstring& value)
    {
        const int required = WideCharToMultiByte(
            CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (required == 0)
        {
            return {};
        }

        std::string result(static_cast<size_t>(required), '\0');
        if (WideCharToMultiByte(
                CP_UTF8, 0, value.c_str(), -1, &result[0], required,
                nullptr, nullptr) == 0)
        {
            return {};
        }

        result.pop_back();
        return result;
    }

    bool isHexString(const std::string& value)
    {
        if (value.empty() || value.size() % 2 != 0)
        {
            return false;
        }

        for (const unsigned char character : value)
        {
            const bool isDigit = character >= '0' && character <= '9';
            const bool isLowerHex = character >= 'a' && character <= 'f';
            const bool isUpperHex = character >= 'A' && character <= 'F';
            if (!isDigit && !isLowerHex && !isUpperHex)
            {
                return false;
            }
        }

        return true;
    }

    bool runRekey(
        const char* databasePath,
        const std::string& hexKey,
        bool encrypt)
    {
        const std::wstring directory = executableDirectory();
        if (directory.empty())
        {
            std::cerr << "Could not determine the executable directory.\n";
            return false;
        }

        const std::wstring corePath = directory + L"Qt6Core.dll";
        const std::wstring sqlPath = directory + L"Qt6Sql.dll";
        const std::wstring pluginPath = directory + L"plugins";
        const std::wstring sqlitePluginPath = pluginPath + L"\\sqldrivers\\qsqlite.dll";
        if (GetFileAttributesW(sqlitePluginPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            std::cerr << "SQLite driver plugin was not found: "
                      << toUtf8(sqlitePluginPath) << '\n';
            return false;
        }

        HMODULE coreModule = LoadLibraryW(corePath.c_str());
        if (coreModule == nullptr)
        {
            std::cerr << "Could not load Qt6Core.dll. Windows error: "
                      << GetLastError() << '\n';
            return false;
        }

        HMODULE sqlModule = LoadLibraryW(sqlPath.c_str());
        if (sqlModule == nullptr)
        {
            std::cerr << "Could not load Qt6Sql.dll. Windows error: "
                      << GetLastError() << '\n';
            FreeLibrary(coreModule);
            return false;
        }

        const QStringFromStdStringFn QString_fromStdString =
            loadFunction<QStringFromStdStringFn>(
                coreModule,
                "?fromStdString@QString@@SA?AV1@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z");
        const QStringDtor QString_dtor =
            loadFunction<QStringDtor>(coreModule, "??1QString@@QEAA@XZ");
        const AddLibraryPathFn addLibraryPath =
            loadFunction<AddLibraryPathFn>(
                coreModule, "?addLibraryPath@QCoreApplication@@SAXAEBVQString@@@Z");
        const CoreApplicationCtor application_ctor =
            loadFunction<CoreApplicationCtor>(
                coreModule, "??0QCoreApplication@@QEAA@AEAHPEAPEADH@Z");
        const CoreApplicationDtor application_dtor =
            loadFunction<CoreApplicationDtor>(
                coreModule, "??1QCoreApplication@@UEAA@XZ");

        const AddDatabaseFn addDatabase =
            loadFunction<AddDatabaseFn>(
                sqlModule, "?addDatabase@QSqlDatabase@@SA?AV1@AEBVQString@@0@Z");
        const SetDatabaseNameFn setDatabaseName =
            loadFunction<SetDatabaseNameFn>(
                sqlModule, "?setDatabaseName@QSqlDatabase@@QEAAXAEBVQString@@@Z");
        const DatabaseOpenFn databaseOpen =
            loadFunction<DatabaseOpenFn>(
                sqlModule, "?open@QSqlDatabase@@QEAA_NXZ");
        const DatabaseCloseFn databaseClose =
            loadFunction<DatabaseCloseFn>(
                sqlModule, "?close@QSqlDatabase@@QEAAXXZ");
        const QueryCtor query_ctor =
            loadFunction<QueryCtor>(
                sqlModule, "??0QSqlQuery@@QEAA@AEBVQString@@AEBVQSqlDatabase@@@Z");
        const QueryExecFn queryExec =
            loadFunction<QueryExecFn>(
                sqlModule, "?exec@QSqlQuery@@QEAA_NAEBVQString@@@Z");
        const QueryFinishFn queryFinish =
            loadFunction<QueryFinishFn>(
                sqlModule, "?finish@QSqlQuery@@QEAAXXZ");
        const SqlObjectDtor query_dtor =
            loadFunction<SqlObjectDtor>(sqlModule, "??1QSqlQuery@@QEAA@XZ");
        const SqlObjectDtor database_dtor =
            loadFunction<SqlObjectDtor>(sqlModule, "??1QSqlDatabase@@QEAA@XZ");

        if (QString_fromStdString == nullptr || QString_dtor == nullptr ||
            addLibraryPath == nullptr || application_ctor == nullptr ||
            application_dtor == nullptr || addDatabase == nullptr ||
            setDatabaseName == nullptr || databaseOpen == nullptr ||
            databaseClose == nullptr || query_ctor == nullptr ||
            queryExec == nullptr || queryFinish == nullptr ||
            query_dtor == nullptr || database_dtor == nullptr)
        {
            std::cerr << "A required Qt function export was not found.\n";
            FreeLibrary(sqlModule);
            FreeLibrary(coreModule);
            return false;
        }

        bool succeeded = false;
        {
            int argc = 1;
            char applicationName[] = "SQLiteEncryptionExtensionEnDe";
            char* argv[] = { applicationName, nullptr };
            CoreApplicationObject application{};
            application_ctor(&application, &argc, argv, 0);

            {
                QString pluginDirectory{};
                const std::string pluginPathUtf8 = toUtf8(pluginPath);
                QString_fromStdString(&pluginDirectory, &pluginPathUtf8);
                addLibraryPath(&pluginDirectory);
                QString_dtor(&pluginDirectory);
            }

            {
                QString driver{};
                QString connectionName{};
                QString databaseName{};
                QString emptyQuery{};
                QString keySql{};
                QString rekeySql{};

                const std::string driverName = "QSQLITE";
                const std::string connection = "rekey-connection";
                const std::string databasePathString = databasePath;
                const std::string noQuery;
                QString_fromStdString(&driver, &driverName);
                QString_fromStdString(&connectionName, &connection);
                QString_fromStdString(&databaseName, &databasePathString);
                QString_fromStdString(&emptyQuery, &noQuery);

                SqlObject database{};
                addDatabase(&database, &driver, &connectionName);
                setDatabaseName(&database, &databaseName);

                if (!databaseOpen(&database))
                {
                    std::cerr << "Opening the database failed.\n";
                    databaseClose(&database);
                }
                else
                {
                    SqlObject query{};
                    query_ctor(&query, &emptyQuery, &database);

                    const std::string keyCommand = encrypt
                        ? "PRAGMA hexrekey = '" + hexKey + "';"
                        : "PRAGMA hexkey = '" + hexKey + "';";
                    QString_fromStdString(&keySql, &keyCommand);

                    if (!queryExec(&query, &keySql))
                    {
                        std::cerr << (encrypt
                            ? "PRAGMA hexrekey failed.\n"
                            : "PRAGMA hexkey failed.\n");
                    }
                    else if (encrypt)
                    {
                        std::cout << "PRAGMA hexrekey executed successfully.\n";
                        succeeded = true;
                    }
                    else
                    {
                        const std::string rekeyCommand = "PRAGMA rekey = '';";
                        QString_fromStdString(&rekeySql, &rekeyCommand);

                        if (!queryExec(&query, &rekeySql))
                        {
                            std::cerr << "PRAGMA rekey failed.\n";
                        }
                        else
                        {
                            std::cout << "PRAGMA hexkey and key removal executed successfully.\n";
                            succeeded = true;
                        }
                    }

                    queryFinish(&query);
                    query_dtor(&query);
                    databaseClose(&database);
                }

                database_dtor(&database);
                QString_dtor(&rekeySql);
                QString_dtor(&keySql);
                QString_dtor(&emptyQuery);
                QString_dtor(&databaseName);
                QString_dtor(&connectionName);
                QString_dtor(&driver);
            }

            application_dtor(&application);
        }

        FreeLibrary(sqlModule);
        FreeLibrary(coreModule);
        return succeeded;
    }
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc < 3 || argc > 4)
    {
        std::cerr << "Usage: SQLiteEncryptionExtensionEnDe.exe <en|de> <hex-key> [database-path]\n";
        return 1;
    }

    const std::string mode = toUtf8(argv[1]);
    if (mode != "en" && mode != "de")
    {
        std::cerr << "Mode must be 'en' (encrypt) or 'de' (decrypt).\n";
        return 1;
    }

    const std::string hexKey = toUtf8(argv[2]);
    if (!isHexString(hexKey))
    {
        std::cerr << "The key must be a non-empty, even-length hexadecimal string.\n";
        return 1;
    }

    const std::string databasePath = argc == 4
        ? toUtf8(argv[3])
        : R"(D:\Database\test.db)";
    if (databasePath.empty())
    {
        std::cerr << "The database path is empty or could not be converted to UTF-8.\n";
        return 1;
    }

    return runRekey(databasePath.c_str(), hexKey, mode == "en") ? 0 : 1;
}
