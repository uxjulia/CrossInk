Import("env")  # noqa: F821 - SCons injects this at build time
import builtins

RUN_SIMULATOR_TARGET_KEY = "_crosspoint_run_simulator_target_registered"


def run_simulator(source, target, env):
    import os
    import subprocess

    binary = env.subst("$BUILD_DIR/program")
    subprocess.run([binary], cwd=os.getcwd())


if not getattr(builtins, RUN_SIMULATOR_TARGET_KEY, False):
    setattr(builtins, RUN_SIMULATOR_TARGET_KEY, True)
    env.AddCustomTarget(
        name="run_simulator",
        dependencies="$PROGPATH",
        actions=run_simulator,
        title="Run Simulator",
        description="Build and run the desktop simulator",
        always_build=True,
    )
