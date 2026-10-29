#pragma once

#include "utils.h"

bool processInputPath(
    InputType inputType,
    const std::wstring& path,
    OperationMode mode,
    const std::string& hexKey,
    std::wstring& error);
