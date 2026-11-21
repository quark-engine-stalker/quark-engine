#include "stdafx.h"
#pragma hdrstop

#include "profiler.h"

struct auth_options
{
	xr_vector<shared_str> ignore;
	xr_vector<shared_str> important;
};

void auth_entry(void* p)
{
	PROF_EVENT();

	FS.auth_runtime(p);
}

void CLocatorAPI::auth_generate(xr_vector<shared_str>& ignore, xr_vector<shared_str>& important)
{
	auth_options* _o = xr_new<auth_options>();
	_o->ignore = ignore;
	_o->important = important;

#if 1
	FS.auth_runtime(_o);
#else
    thread_spawn(auth_entry, "checksum", 0, _o);
#endif
}

u64 CLocatorAPI::auth_get()
{
	m_auth_lock.Enter();
	m_auth_lock.Leave();
	return m_auth_code;
}

void CLocatorAPI::auth_runtime(void* params)
{
	PROF_EVENT();

	m_auth_lock.Enter();
	auth_options* _o = (auth_options*)params;

	CMemoryWriter writer;
	pSettingsAuth->save_as(writer);
	m_auth_code = crc32(writer.pointer(), writer.size());

#ifdef DEBUG
    if (strstr(Core.Params, "auth_debug"))
    {
        string_path tmp_path;
        update_path(tmp_path, "$app_data_root$", "auth_psettings.ltx");
        IWriter* tmp_dst = w_open(tmp_path);
        pSettingsAuth->save_as(*tmp_dst, false);
        w_close(tmp_dst);
    }
#endif

#ifdef DEBUG
    bool b_extern_auth = !!strstr(Core.Params, "asdf");
    if (!b_extern_auth)
#endif // DEBUG
	{
		// r_open() needs the same index lock, so copy names before hashing.
		xr_vector<xr_string> to_hash;
		{
			xrSRWLockGuard guard(m_files_lock, true);
			for (const file& f : m_files)
			{
				if (!f.size_real)
					continue;

				const auto should_skip = [&f](const shared_str& name)
				{
					return strstr(f.name, name.c_str()) != nullptr;
				};
				if (std::any_of(_o->ignore.begin(), _o->ignore.end(), should_skip))
					continue;

				for (const shared_str& name : _o->important)
					if (strstr(f.name, name.c_str()))
						to_hash.emplace_back(f.name);
			}
		}

		for (const xr_string& name : to_hash)
		{
			IReader* r = FS.r_open(name.c_str());
			if (!r)
				break;
			u32 crc = crc32(r->pointer(), r->length());
#ifdef DEBUG
            if (strstr(Core.Params, "auth_debug"))
                Msg("auth %s = 0x%08x", name.c_str(), crc);
#endif
			FS.r_close(r);
			m_auth_code ^= u64(crc);
		}
#ifdef DEBUG
        Msg("auth_code = %d", m_auth_code);
#endif // DEBUG
	}
#ifdef DEBUG
    else
    {
        string64 c_auth_code;
        sscanf(strstr(Core.Params, "asdf ") + 5, "%[^ ] ", c_auth_code);
        m_auth_code = _atoi64(c_auth_code);
    }
#endif // DEBUG
	xr_delete(_o);

	m_auth_lock.Leave();
}
