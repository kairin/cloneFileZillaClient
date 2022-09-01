
#include "serverdata.h"
#include "loginmanager.h"
#include "Options.h"

#include "../commonui/protect.h"

namespace {
wxColour const background_colors[] = {
	wxColour(),
	wxColour(255, 0, 0, 32),
	wxColour(0, 255, 0, 32),
	wxColour(0, 0, 255, 32),
	wxColour(255, 255, 0, 32),
	wxColour(0, 255, 255, 32),
	wxColour(255, 0, 255, 32),
	wxColour(255, 128, 0, 32) };
}

wxColor site_colour_to_wx(site_colour c)
{
	auto index = static_cast<size_t>(c);
	if (index < sizeof(background_colors) / sizeof(*background_colors)){
		return background_colors[index];
	}
	return background_colors[0];
}

void protect(ProtectedCredentials& creds)
{
	protect(creds, CLoginManager::Get(), *COptions::Get());
}
