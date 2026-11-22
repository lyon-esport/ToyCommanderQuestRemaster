// tcvr-web: functions the libretro browser build does not have yet (no netplay match states, no
// light gun over UDP). Step 5 of the browser plan (multiplayer) replaces the match-state ones.
#if defined(__EMSCRIPTEN__) && defined(LIBRETRO)
#include "types.h"
#include <string>
#include <vector>

bool dc_serializeAll(std::vector<u8>& out)
{
	(void)out;
	return false;
}

bool dc_writeStateFile(const std::string& path, const std::vector<u8>& state)
{
	(void)path;
	(void)state;
	return false;
}

void lightgunSet(int player, int x, int y, u32 buttons)
{
	(void)player; (void)x; (void)y; (void)buttons;
}
#endif
