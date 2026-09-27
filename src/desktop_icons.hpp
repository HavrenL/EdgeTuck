#pragma once
#include "model.hpp"
#include <windows.h>
namespace edge {
enum class ParkResult { Parked, AutoArrange, NotOnDesktop, Unavailable };
ParkResult park_folder_icon(DrawerModel& drawer);
bool restore_folder_icon(DrawerModel& drawer);
bool desktop_icon_event(HWND window, LONG object);
}
