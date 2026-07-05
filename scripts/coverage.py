Import("env")

env.Append(
    CCFLAGS=["--coverage", "-fprofile-update=prefer-atomic"],
    LINKFLAGS=["--coverage"]
)
