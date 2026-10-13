#pragma once
#include "utils/nt.hpp"
#include "launcher/launcher.hpp"

class loader final
{
public:
	explicit loader(launcher::mode mode);

	FARPROC load(const utils::nt::module& module) const;

	void set_import_resolver(const std::function<FARPROC(const std::string&, const std::string&)>& resolver);

private:
	launcher::mode mode_;
	std::function<FARPROC(const std::string&, const std::string&)> import_resolver_;

	static void load_section(const utils::nt::module& target, const utils::nt::module& source,
	                         IMAGE_SECTION_HEADER* section);
	void load_sections(const utils::nt::module& target, const utils::nt::module& source) const;
	void load_imports(const utils::nt::module& target, const utils::nt::module& source) const;
};
