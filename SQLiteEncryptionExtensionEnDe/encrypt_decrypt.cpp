#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "encrypt_decrypt.h"

#include <cstdint>
#include <sstream>
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
    using RemoveDatabaseFn = void (__fastcall*)(const QString*);
    using SetDatabaseNameFn = void (__fastcall*)(SqlObject*, const QString*);
    using DatabaseOpenFn = bool (__fastcall*)(SqlObject*);
    using DatabaseCloseFn = void (__fastcall*)(SqlObject*);
    using QueryCtor = SqlObject* (__fastcall*)(
        SqlObject*, const QString*, const SqlObject*);
    using QueryExecFn = bool (__fastcall*)(SqlObject*, const QString*);
    using QueryFinishFn = void (__fastcall*)(SqlObject*);
    using SqlObjectDtor = void (__fastcall*)(SqlObject*);

    struct QtRuntime
    {
        HMODULE coreModule = nullptr;
        HMODULE sqlModule = nullptr;
        QStringFromStdStringFn QString_fromStdString = nullptr;
        QStringDtor QString_dtor = nullptr;
        AddLibraryPathFn addLibraryPath = nullptr;
        CoreApplicationCtor application_ctor = nullptr;
        CoreApplicationDtor application_dtor = nullptr;
        AddDatabaseFn addDatabase = nullptr;
        RemoveDatabaseFn removeDatabase = nullptr;
        SetDatabaseNameFn setDatabaseName = nullptr;
        DatabaseOpenFn databaseOpen = nullptr;
        DatabaseCloseFn databaseClose = nullptr;
        QueryCtor query_ctor = nullptr;
        QueryExecFn queryExec = nullptr;
        QueryFinishFn queryFinish = nullptr;
        SqlObjectDtor query_dtor = nullptr;
        SqlObjectDtor database_dtor = nullptr;
        CoreApplicationObject application{};
        int applicationArgc = 1;
        char applicationName[40] = "SQLiteEncryptionExtensionEnDe";
        char* applicationArgv[2] = { applicationName, nullptr };
        bool applicationInitialized = false;
        std::uint64_t connectionNumber = 0;
    };

    QtRuntime runtime;

    template <typename Function>
    Function loadFunction(HMODULE module, const char* name)
    {
        return reinterpret_cast<Function>(GetProcAddress(module, name));
    }

    std::wstring win32Error(const wchar_t* action, const std::wstring& path)
    {
        std::wostringstream message;
        message << action << L" '" << path << L"' (Windows error "
                << GetLastError() << L").";
        return message.str();
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

    void resetRuntime()
    {
        if (runtime.applicationInitialized)
        {
            runtime.application_dtor(&runtime.application);
        }
        runtime.applicationInitialized = false;

        if (runtime.sqlModule != nullptr)
        {
            FreeLibrary(runtime.sqlModule);
        }
        if (runtime.coreModule != nullptr)
        {
            FreeLibrary(runtime.coreModule);
        }
        runtime = {};
    }

    void convertToQString(const std::string& value, QString& result)
    {
        runtime.QString_fromStdString(&result, &value);
    }
}

bool initializeQtRuntime(std::wstring& error)
{
    if (runtime.applicationInitialized)
    {
        error = L"Qt runtime is already initialized.";
        return false;
    }

    const std::wstring directory = executableDirectory();
    if (directory.empty())
    {
        error = L"Could not determine the executable directory.";
        return false;
    }

    const std::wstring corePath = directory + L"Qt6Core.dll";
    const std::wstring sqlPath = directory + L"Qt6Sql.dll";
    const std::wstring pluginPath = directory + L"plugins";
    const std::wstring sqlitePluginPath =
        pluginPath + L"\\sqldrivers\\qsqlite.dll";
    if (GetFileAttributesW(sqlitePluginPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        error = win32Error(L"SQLite driver plugin was not found", sqlitePluginPath);
        return false;
    }

    runtime.coreModule = LoadLibraryW(corePath.c_str());
    if (runtime.coreModule == nullptr)
    {
        error = win32Error(L"Could not load Qt6Core.dll", corePath);
        resetRuntime();
        return false;
    }

    runtime.sqlModule = LoadLibraryW(sqlPath.c_str());
    if (runtime.sqlModule == nullptr)
    {
        error = win32Error(L"Could not load Qt6Sql.dll", sqlPath);
        resetRuntime();
        return false;
    }

    runtime.QString_fromStdString = loadFunction<QStringFromStdStringFn>(
        runtime.coreModule,
        "?fromStdString@QString@@SA?AV1@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z");
    runtime.QString_dtor = loadFunction<QStringDtor>(
        runtime.coreModule, "??1QString@@QEAA@XZ");
    runtime.addLibraryPath = loadFunction<AddLibraryPathFn>(
        runtime.coreModule,
        "?addLibraryPath@QCoreApplication@@SAXAEBVQString@@@Z");
    runtime.application_ctor = loadFunction<CoreApplicationCtor>(
        runtime.coreModule, "??0QCoreApplication@@QEAA@AEAHPEAPEADH@Z");
    runtime.application_dtor = loadFunction<CoreApplicationDtor>(
        runtime.coreModule, "??1QCoreApplication@@UEAA@XZ");

    runtime.addDatabase = loadFunction<AddDatabaseFn>(
        runtime.sqlModule,
        "?addDatabase@QSqlDatabase@@SA?AV1@AEBVQString@@0@Z");
    runtime.removeDatabase = loadFunction<RemoveDatabaseFn>(
        runtime.sqlModule, "?removeDatabase@QSqlDatabase@@SAXAEBVQString@@@Z");
    runtime.setDatabaseName = loadFunction<SetDatabaseNameFn>(
        runtime.sqlModule,
        "?setDatabaseName@QSqlDatabase@@QEAAXAEBVQString@@@Z");
    runtime.databaseOpen = loadFunction<DatabaseOpenFn>(
        runtime.sqlModule, "?open@QSqlDatabase@@QEAA_NXZ");
    runtime.databaseClose = loadFunction<DatabaseCloseFn>(
        runtime.sqlModule, "?close@QSqlDatabase@@QEAAXXZ");
    runtime.query_ctor = loadFunction<QueryCtor>(
        runtime.sqlModule,
        "??0QSqlQuery@@QEAA@AEBVQString@@AEBVQSqlDatabase@@@Z");
    runtime.queryExec = loadFunction<QueryExecFn>(
        runtime.sqlModule, "?exec@QSqlQuery@@QEAA_NAEBVQString@@@Z");
    runtime.queryFinish = loadFunction<QueryFinishFn>(
        runtime.sqlModule, "?finish@QSqlQuery@@QEAAXXZ");
    runtime.query_dtor = loadFunction<SqlObjectDtor>(
        runtime.sqlModule, "??1QSqlQuery@@QEAA@XZ");
    runtime.database_dtor = loadFunction<SqlObjectDtor>(
        runtime.sqlModule, "??1QSqlDatabase@@QEAA@XZ");

    if (runtime.QString_fromStdString == nullptr ||
        runtime.QString_dtor == nullptr || runtime.addLibraryPath == nullptr ||
        runtime.application_ctor == nullptr || runtime.application_dtor == nullptr ||
        runtime.addDatabase == nullptr || runtime.removeDatabase == nullptr ||
        runtime.setDatabaseName == nullptr || runtime.databaseOpen == nullptr ||
        runtime.databaseClose == nullptr || runtime.query_ctor == nullptr ||
        runtime.queryExec == nullptr || runtime.queryFinish == nullptr ||
        runtime.query_dtor == nullptr || runtime.database_dtor == nullptr)
    {
        error = L"A required Qt function export was not found.";
        resetRuntime();
        return false;
    }

    runtime.application_ctor(
        &runtime.application,
        &runtime.applicationArgc,
        runtime.applicationArgv,
        0);
    runtime.applicationInitialized = true;

    const std::string pluginPathUtf8 = toUtf8(pluginPath);
    if (pluginPathUtf8.empty())
    {
        error = L"Could not convert the Qt plugin path to UTF-8.";
        resetRuntime();
        return false;
    }

    QString pluginDirectory{};
    convertToQString(pluginPathUtf8, pluginDirectory);
    runtime.addLibraryPath(&pluginDirectory);
    runtime.QString_dtor(&pluginDirectory);
    return true;
}

void clearQtRuntime()
{
    resetRuntime();
}

bool executeCipherPragmas(
    const std::wstring& databasePath,
    OperationMode mode,
    const std::string& hexKey,
    std::wstring& error)
{
    if (!runtime.applicationInitialized)
    {
        error = L"Qt runtime has not been initialized.";
        return false;
    }

    const std::string databasePathUtf8 = toUtf8(databasePath);
    if (databasePathUtf8.empty())
    {
        error = L"Could not convert the database path to UTF-8.";
        return false;
    }

    QString driver{};
    QString connectionName{};
    QString databaseName{};
    QString emptyQuery{};
    QString keySql{};
    QString rekeySql{};

    const std::string driverName = "QSQLITE";
    const std::string connection =
        "sqlite-cipher-" + std::to_string(++runtime.connectionNumber);
    const std::string noQuery;
    convertToQString(driverName, driver);
    convertToQString(connection, connectionName);
    convertToQString(databasePathUtf8, databaseName);
    convertToQString(noQuery, emptyQuery);

    SqlObject database{};
    runtime.addDatabase(&database, &driver, &connectionName);
    runtime.setDatabaseName(&database, &databaseName);

    bool succeeded = false;
    if (!runtime.databaseOpen(&database))
    {
        error = mode == OperationMode::Decrypt
            ? L"Could not open database with the QSQLITE driver. The key may be incorrect or the cipher format may be incompatible."
            : L"Could not open database with the QSQLITE driver.";
        runtime.databaseClose(&database);
    }
    else
    {
        SqlObject query{};
        runtime.query_ctor(&query, &emptyQuery, &database);

        std::string keyCommand =
            mode == OperationMode::Encrypt
            ? "PRAGMA hexrekey = '" + hexKey + "';"
            : "PRAGMA hexkey = '" + hexKey + "';";
        convertToQString(keyCommand, keySql);
        secureClear(keyCommand);

        if (!runtime.queryExec(&query, &keySql))
        {
            error = mode == OperationMode::Encrypt
                ? L"PRAGMA hexrekey failed."
                : L"PRAGMA hexkey failed.";
        }
        else if (mode == OperationMode::Encrypt)
        {
            succeeded = true;
        }
        else
        {
            const std::string rekeyCommand = "PRAGMA rekey = '';";
            convertToQString(rekeyCommand, rekeySql);
            if (!runtime.queryExec(&query, &rekeySql))
            {
                error = L"PRAGMA rekey failed.";
            }
            else
            {
                succeeded = true;
            }
        }

        runtime.queryFinish(&query);
        runtime.query_dtor(&query);
        runtime.databaseClose(&database);
    }

    runtime.database_dtor(&database);
    runtime.removeDatabase(&connectionName);
    runtime.QString_dtor(&rekeySql);
    runtime.QString_dtor(&keySql);
    runtime.QString_dtor(&emptyQuery);
    runtime.QString_dtor(&databaseName);
    runtime.QString_dtor(&connectionName);
    runtime.QString_dtor(&driver);
    return succeeded;
}
