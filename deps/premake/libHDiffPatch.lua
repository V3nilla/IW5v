libHDiffPatch = {
	source = path.join(dependencies.basePath, "HDiffPatch/libHDiffPatch"),
}

function libHDiffPatch.import()
	links {
		"libHDiffPatch"
	}

	libHDiffPatch.includes()
end

function libHDiffPatch.includes()
	includedirs {
		path.join(libHDiffPatch.source, "HDiff"),
		path.join(libHDiffPatch.source, "HPatch"),
	}
end

function libHDiffPatch.project()
	project "libHDiffPatch"
		language "C"

		libHDiffPatch.includes()

		files {
			path.join(libHDiffPatch.source, "HDiff/**.h"),
			path.join(libHDiffPatch.source, "HDiff/**.c"),
			path.join(libHDiffPatch.source, "HDiff/**.cpp"),

			path.join(libHDiffPatch.source, "HPatch/**.h"),
			path.join(libHDiffPatch.source, "HPatch/**.c"),
		}

		warnings "Off"
		kind "StaticLib"
end

table.insert(dependencies, libHDiffPatch)
