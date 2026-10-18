# default Godot API version; api_version=<ver> on the command line still overrides it
api_version = "4.7"
Export("api_version")

# godot-cpp defaults use_hot_reload to False; without it, live instances can't be recreated on reload.
# use_hot_reload=no on the command line still overrides it
env = Environment(tools=["default"], PLATFORM="", use_hot_reload=True)
Export("env")

env = SConscript("godot-cpp/SConstruct", exports="env")
env.Append(CPPPATH=["src/"])

build_dir = "build/{}".format(env["suffix"].lstrip("."))
VariantDir(build_dir, "src", duplicate=False)

lib = env.SharedLibrary(
    "demo/bin/cuber{}{}".format(env["suffix"], env["SHLIBSUFFIX"]),
    source=Glob(build_dir + "/*.cpp"),
)
Default(lib)