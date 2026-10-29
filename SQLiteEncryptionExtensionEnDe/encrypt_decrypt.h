#pragma once

#include "utils.h"

bool initializeQtRuntime(std::wstring& error);
void clearQtRuntime();

bool executeCipherPragmas(
    const std::wstring& databasePath,
    OperationMode mode,
    const std::string& hexKey,
    std::wstring& error);
