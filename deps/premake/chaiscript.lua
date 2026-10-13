chaiscript = {
	source = path.join(dependencies.basePath, "ChaiScript"),
}

function chaiscript.import()
	chaiscript.includes()

	defines {
		--"CHAISCRIPT_NO_THREADS" -- :(
	}
end

function chaiscript.includes()
	includedirs {
		path.join(chaiscript.source, "include"),
	}
end

function chaiscript.project()

end

table.insert(dependencies, chaiscript)
