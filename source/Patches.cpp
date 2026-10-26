#include "PCH.h"

#include "Patches.h"

#include "utils/Logger.h"

namespace Patches
{
	namespace
	{
		struct Entry
		{
			Group group;
			Installer installer;
			Wanted wanted;
		};

		std::vector<Entry> entries;
		std::vector<Group> published;
		bool installed = false;

		void Publish()
		{
			published.clear();
			published.reserve(entries.size());
			for (const auto& e : entries) { published.push_back(e.group); }
		}
	}

	void Register(std::string a_name, std::string a_touches, Installer a_installer, Wanted a_wanted)
	{
		Entry e;
		e.group.name = std::move(a_name);
		e.group.touches = std::move(a_touches);
		e.group.status = "not installed yet";
		e.installer = std::move(a_installer);
		e.wanted = std::move(a_wanted);
		entries.push_back(std::move(e));
		Publish();
	}

	void InstallAll()
	{
		if (installed) { return; }
		installed = true;

		for (auto& e : entries)
		{
			std::string reason;
			bool ok = false;
			try
			{
				ok = e.installer ? e.installer(reason) : false;
			}
			catch (const std::exception& ex)
			{
				ok = false;
				reason = std::string("the installer threw: ") + ex.what();
			}
			catch (...)
			{
				ok = false;
				reason = "the installer threw";
			}

			e.group.installed = ok;
			e.group.status = ok ? "installed" : (reason.empty() ? "not installed" : reason);
			e.group.refused = !ok && e.group.status.rfind("refused", 0) == 0;
			e.group.alongsideCsf = ok && reason.find("alongside Custom Skills Framework") != std::string::npos;

			if (ok)
			{
				logger::info("patch group \"{}\" installed", e.group.name);
			}
			else
			{
				// Deliberately a warning, not an error: the game is fine, that feature is simply
				// not active, and the player is told rather than left guessing.
				logger::warn("patch group \"{}\" is NOT active - {}. That part of the game behaves "
							 "exactly as it would without this mod.",
							 e.group.name, e.group.status);
			}
		}
		Publish();
	}

	const std::vector<Group>& All() { return published; }

	std::vector<std::string> RefusedAndWanted()
	{
		std::vector<std::string> out;
		for (const auto& e : entries)
		{
			if (e.group.refused && (!e.wanted || e.wanted())) { out.push_back(e.group.name); }
		}
		return out;
	}

	bool IsInstalled(const std::string& a_name)
	{
		for (const auto& e : entries)
		{
			if (e.group.name == a_name) { return e.group.installed; }
		}
		return false;
	}
}
