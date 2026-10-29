#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "database_files.h"
#include "encrypt_decrypt.h"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <sstream>
#include <utility>
#include <vector>

namespace
{
    constexpr unsigned char SqliteHeader[] = {
        'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f',
        'o', 'r', 'm', 'a', 't', ' ', '3', '\0'
    };

    struct FileSnapshot
    {
        std::wstring sourcePath;
        std::wstring backupPath;
        std::wstring workingPath;
        bool existed = false;
    };

    std::wstring win32Error(const wchar_t* action, const std::wstring& path)
    {
        std::wostringstream message;
        message << action << L" '" << path << L"' (Windows error "
                << GetLastError() << L").";
        return message.str();
    }

    class SourceFileLock
    {
    public:
        ~SourceFileLock()
        {
            if (handle_ != INVALID_HANDLE_VALUE)
            {
                CloseHandle(handle_);
            }
        }

        SourceFileLock() = default;
        SourceFileLock(const SourceFileLock&) = delete;
        SourceFileLock& operator=(const SourceFileLock&) = delete;

        bool acquire(const std::wstring& path, std::wstring& error)
        {
            handle_ = CreateFileW(
                path.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle_ == INVALID_HANDLE_VALUE)
            {
                error = win32Error(
                    L"Could not lock file; close applications using it", path);
                return false;
            }
            return true;
        }

    private:
        HANDLE handle_ = INVALID_HANDLE_VALUE;
    };

    bool inspectFile(const std::wstring& path, bool& exists, std::wstring& error)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
        {
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                error = L"Expected a file but found a folder: '" + path + L"'.";
                return false;
            }
            exists = true;
            return true;
        }

        const DWORD lastError = GetLastError();
        if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND)
        {
            exists = false;
            return true;
        }

        SetLastError(lastError);
        error = win32Error(L"Could not inspect file", path);
        return false;
    }

    bool inspectSqliteHeader(
        const std::wstring& path,
        bool& hasSqliteHeader,
        std::wstring& error)
    {
        HANDLE file = CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            error = win32Error(L"Could not read database header", path);
            return false;
        }

        unsigned char header[sizeof(SqliteHeader)]{};
        DWORD bytesRead = 0;
        const BOOL succeeded = ReadFile(
            file, header, static_cast<DWORD>(sizeof(header)), &bytesRead, nullptr);
        const DWORD readError = succeeded ? ERROR_SUCCESS : GetLastError();
        CloseHandle(file);

        if (!succeeded)
        {
            SetLastError(readError);
            error = win32Error(L"Could not read database header", path);
            return false;
        }
        if (bytesRead != sizeof(header))
        {
            error = L"File is too small to contain a SQLite database header: '" +
                path + L"'.";
            return false;
        }

        hasSqliteHeader = std::equal(
            std::begin(SqliteHeader), std::end(SqliteHeader), std::begin(header));
        return true;
    }

    bool deleteIfExists(const std::wstring& path, std::wstring& error)
    {
        bool exists = false;
        if (!inspectFile(path, exists, error))
        {
            return false;
        }
        if (!exists)
        {
            return true;
        }
        if (!DeleteFileW(path.c_str()))
        {
            error = win32Error(L"Could not remove file", path);
            return false;
        }
        return true;
    }

    void cleanupFiles(
        const std::vector<FileSnapshot>& snapshots,
        bool backups,
        std::wstring& error)
    {
        for (const FileSnapshot& snapshot : snapshots)
        {
            std::wstring cleanupError;
            if (!deleteIfExists(
                    backups ? snapshot.backupPath : snapshot.workingPath,
                    cleanupError))
            {
                if (!error.empty())
                {
                    error += L" ";
                }
                error += cleanupError;
            }
        }
    }

    bool restoreSnapshot(FileSnapshot& snapshot, std::wstring& error)
    {
        bool sourceExists = false;
        if (!inspectFile(snapshot.sourcePath, sourceExists, error))
        {
            return false;
        }

        if (!snapshot.existed)
        {
            if (sourceExists && !DeleteFileW(snapshot.sourcePath.c_str()))
            {
                error = win32Error(L"Could not remove file during rollback", snapshot.sourcePath);
                return false;
            }
            return true;
        }

        if (sourceExists)
        {
            if (!ReplaceFileW(
                    snapshot.sourcePath.c_str(), snapshot.backupPath.c_str(),
                    nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr))
            {
                error = win32Error(L"Could not restore backup", snapshot.sourcePath);
                return false;
            }
        }
        else if (!MoveFileExW(
                     snapshot.backupPath.c_str(), snapshot.sourcePath.c_str(),
                     MOVEFILE_WRITE_THROUGH))
        {
            error = win32Error(L"Could not restore backup", snapshot.sourcePath);
            return false;
        }
        return true;
    }

    bool rollbackSnapshots(
        std::vector<FileSnapshot>& snapshots,
        std::wstring& error)
    {
        bool succeeded = true;
        for (auto snapshot = snapshots.rbegin(); snapshot != snapshots.rend(); ++snapshot)
        {
            std::wstring restoreError;
            if (!restoreSnapshot(*snapshot, restoreError))
            {
                if (!error.empty())
                {
                    error += L" ";
                }
                error += restoreError;
                succeeded = false;
            }
        }
        return succeeded;
    }

    bool commitSnapshot(FileSnapshot& snapshot, std::wstring& error)
    {
        bool workingExists = false;
        if (!inspectFile(snapshot.workingPath, workingExists, error))
        {
            return false;
        }
        if (!workingExists)
        {
            return !snapshot.existed || deleteIfExists(snapshot.sourcePath, error);
        }

        bool sourceExists = false;
        if (!inspectFile(snapshot.sourcePath, sourceExists, error))
        {
            return false;
        }
        if (sourceExists)
        {
            if (!ReplaceFileW(
                    snapshot.sourcePath.c_str(), snapshot.workingPath.c_str(),
                    nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr))
            {
                error = win32Error(L"Could not replace converted file", snapshot.sourcePath);
                return false;
            }
        }
        else if (!MoveFileExW(
                     snapshot.workingPath.c_str(), snapshot.sourcePath.c_str(),
                     MOVEFILE_WRITE_THROUGH))
        {
            error = win32Error(L"Could not install converted sidecar", snapshot.sourcePath);
            return false;
        }
        return true;
    }

    bool reserveTemporaryPaths(
        const std::wstring& source,
        std::wstring& backupPath,
        std::wstring& workingPath,
        std::wstring& error)
    {
        static unsigned long long counter = 0;
        for (unsigned int attempt = 0; attempt < 100; ++attempt)
        {
            std::wostringstream suffix;
            suffix << L".sqlitecipher." << GetCurrentProcessId()
                   << L"." << ++counter;
            backupPath = source + suffix.str() + L".backup";
            workingPath = source + suffix.str() + L".working";

            bool available = true;
            for (const wchar_t* sidecar : { L"", L"-wal", L"-shm" })
            {
                bool exists = false;
                std::wstring inspectError;
                if (!inspectFile(backupPath + sidecar, exists, inspectError) ||
                    exists ||
                    !inspectFile(workingPath + sidecar, exists, inspectError) ||
                    exists)
                {
                    available = false;
                    break;
                }
            }
            if (available)
            {
                return true;
            }
        }

        error = L"Could not reserve unique temporary paths beside the database.";
        return false;
    }

    bool processDatabase(
        const std::wstring& databasePath,
        OperationMode mode,
        const std::string& hexKey,
        std::wstring& error)
    {
        const DWORD attributes = GetFileAttributesW(databasePath.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            error = win32Error(L"Database file is unavailable", databasePath);
            return false;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            error = L"Expected a database file, but the path is a folder.";
            return false;
        }

        SourceFileLock databaseLock;
        if (!databaseLock.acquire(databasePath, error))
        {
            return false;
        }

        bool sourceIsPlaintext = false;
        if (!inspectSqliteHeader(databasePath, sourceIsPlaintext, error))
        {
            return false;
        }
        if (mode == OperationMode::Encrypt && !sourceIsPlaintext)
        {
            error = L"Encryption requires a plaintext SQLite database; the file is encrypted or has an invalid header.";
            return false;
        }
        if (mode == OperationMode::Decrypt && sourceIsPlaintext)
        {
            error = L"Database already has the standard SQLite header; it appears decrypted already.";
            return false;
        }

        const std::wstring journalPath = databasePath + L"-journal";
        bool journalExists = false;
        if (!inspectFile(journalPath, journalExists, error))
        {
            return false;
        }
        if (journalExists)
        {
            error = L"SQLite rollback journal exists: '" + journalPath +
                L"'. Recover the database before conversion.";
            return false;
        }

        std::wstring backupPath;
        std::wstring workingPath;
        if (!reserveTemporaryPaths(databasePath, backupPath, workingPath, error))
        {
            return false;
        }

        std::vector<FileSnapshot> snapshots{
            { databasePath, backupPath, workingPath, true },
            { databasePath + L"-wal", backupPath + L"-wal", workingPath + L"-wal", false },
            { databasePath + L"-shm", backupPath + L"-shm", workingPath + L"-shm", false }
        };
        for (size_t index = 1; index < snapshots.size(); ++index)
        {
            if (!inspectFile(snapshots[index].sourcePath, snapshots[index].existed, error))
            {
                return false;
            }
        }

        SourceFileLock walLock;
        SourceFileLock shmLock;
        if ((snapshots[1].existed && !walLock.acquire(snapshots[1].sourcePath, error)) ||
            (snapshots[2].existed && !shmLock.acquire(snapshots[2].sourcePath, error)))
        {
            return false;
        }

        for (const FileSnapshot& snapshot : snapshots)
        {
            if (snapshot.existed &&
                !CopyFileW(snapshot.sourcePath.c_str(), snapshot.backupPath.c_str(), TRUE))
            {
                error = win32Error(L"Could not create database/sidecar backup", snapshot.backupPath);
                cleanupFiles(snapshots, true, error);
                return false;
            }
        }
        for (size_t index = 0; index < 2; ++index)
        {
            const FileSnapshot& snapshot = snapshots[index];
            if (snapshot.existed &&
                !CopyFileW(snapshot.sourcePath.c_str(), snapshot.workingPath.c_str(), TRUE))
            {
                error = win32Error(L"Could not create working database copy", snapshot.workingPath);
                cleanupFiles(snapshots, false, error);
                cleanupFiles(snapshots, true, error);
                return false;
            }
        }

        if (!executeCipherPragmas(workingPath, mode, hexKey, error))
        {
            cleanupFiles(snapshots, false, error);
            if (mode == OperationMode::Decrypt)
            {
                std::wstring rollbackError;
                if (rollbackSnapshots(snapshots, rollbackError))
                {
                    error += L" Original database and sidecars were restored from backup.";
                }
                else
                {
                    error += L" Rollback was incomplete: " + rollbackError +
                        L" Backups are retained beside the database.";
                }
            }
            else
            {
                error += L" Original database was not changed; backup is at '" +
                    backupPath + L"'.";
            }
            return false;
        }

        bool convertedIsPlaintext = false;
        if (!inspectSqliteHeader(workingPath, convertedIsPlaintext, error))
        {
            cleanupFiles(snapshots, false, error);
            if (mode == OperationMode::Decrypt)
            {
                std::wstring rollbackError;
                if (rollbackSnapshots(snapshots, rollbackError))
                {
                    error += L" Original database and sidecars were restored from backup.";
                }
                else
                {
                    error += L" Rollback was incomplete: " + rollbackError +
                        L" Backups are retained beside the database.";
                }
            }
            else
            {
                error += L" Original database was not changed; backup is at '" +
                    backupPath + L"'.";
            }
            return false;
        }

        const bool conversionVerified =
            mode == OperationMode::Decrypt ? convertedIsPlaintext : !convertedIsPlaintext;
        if (!conversionVerified)
        {
            error = mode == OperationMode::Decrypt
                ? L"Decryption verification failed: converted file does not have the standard SQLite header. The key may be incorrect or the cipher format incompatible."
                : L"Encryption verification failed: converted file still has the standard SQLite header.";
            cleanupFiles(snapshots, false, error);

            if (mode == OperationMode::Decrypt)
            {
                std::wstring rollbackError;
                if (rollbackSnapshots(snapshots, rollbackError))
                {
                    error += L" Original database and sidecars were restored from backup.";
                }
                else
                {
                    error += L" Rollback was incomplete: " + rollbackError +
                        L" Backups are retained beside the database.";
                }
            }
            else
            {
                error += L" Original database was not changed; backup is at '" +
                    backupPath + L"'.";
            }
            return false;
        }

        std::wstring commitError;
        bool committed = commitSnapshot(snapshots[0], commitError) &&
            commitSnapshot(snapshots[1], commitError) &&
            deleteIfExists(snapshots[2].sourcePath, commitError);
        if (!committed)
        {
            error = commitError;
            std::wstring rollbackError;
            if (rollbackSnapshots(snapshots, rollbackError))
            {
                error += L" Original database and sidecars were restored from backup.";
            }
            else
            {
                error += L" Rollback was incomplete: " + rollbackError +
                    L" Backups are retained beside the database.";
            }
            return false;
        }

        for (const FileSnapshot& snapshot : snapshots)
        {
            if (snapshot.existed && !DeleteFileW(snapshot.backupPath.c_str()))
            {
                error = win32Error(
                    L"Conversion succeeded, but backup could not be removed",
                    snapshot.backupPath);
                return false;
            }
        }
        return true;
    }

    bool resolveDatabaseFiles(
        InputType inputType,
        const std::wstring& path,
        std::vector<std::wstring>& files,
        std::wstring& error)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            error = win32Error(L"Path does not exist", path);
            return false;
        }

        const bool isDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (inputType == InputType::File)
        {
            if (isDirectory)
            {
                error = L"Input type is 'file' but the path is a folder.";
                return false;
            }
            files.push_back(path);
            return true;
        }
        if (!isDirectory)
        {
            error = L"Input type is 'folder' but the path is a file.";
            return false;
        }

        std::wstring searchPath = path;
        if (!searchPath.empty() && searchPath.back() != L'\\' && searchPath.back() != L'/')
        {
            searchPath.push_back(L'\\');
        }
        searchPath += L"*.db";

        WIN32_FIND_DATAW entry{};
        HANDLE search = FindFirstFileW(searchPath.c_str(), &entry);
        if (search == INVALID_HANDLE_VALUE)
        {
            if (GetLastError() == ERROR_FILE_NOT_FOUND)
            {
                error = L"No *.db files were found in the folder.";
            }
            else
            {
                error = win32Error(L"Could not search folder", path);
            }
            return false;
        }

        do
        {
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                std::wstring filePath = path;
                if (!filePath.empty() && filePath.back() != L'\\' && filePath.back() != L'/')
                {
                    filePath.push_back(L'\\');
                }
                filePath += entry.cFileName;
                files.push_back(std::move(filePath));
            }
        } while (FindNextFileW(search, &entry));

        const DWORD findError = GetLastError();
        FindClose(search);
        if (findError != ERROR_NO_MORE_FILES)
        {
            SetLastError(findError);
            error = win32Error(L"Error while searching folder", path);
            return false;
        }
        if (files.empty())
        {
            error = L"No *.db files were found in the folder.";
            return false;
        }
        return true;
    }
}

bool processInputPath(
    InputType inputType,
    const std::wstring& path,
    OperationMode mode,
    const std::string& hexKey,
    std::wstring& error)
{
    std::vector<std::wstring> databaseFiles;
    if (!resolveDatabaseFiles(inputType, path, databaseFiles, error))
    {
        return false;
    }

    bool succeeded = true;
    for (const std::wstring& databasePath : databaseFiles)
    {
        std::wcout << L"Processing: " << databasePath << L'\n';
        if (!processDatabase(databasePath, mode, hexKey, error))
        {
            std::wcerr << L"Failed: " << error << L'\n';
            succeeded = false;
        }
        else
        {
            std::wcout << L"Completed: " << databasePath << L'\n';
        }
        error.clear();
    }
    return succeeded;
}
