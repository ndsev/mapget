// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#ifdef GRIDSOURCE_WITH_DEVUI

#include <cstdint>

namespace mapget::gridsource {

void startDevUIServer(uint16_t port);
void stopDevUIServer();

}  // namespace mapget::gridsource

#endif
