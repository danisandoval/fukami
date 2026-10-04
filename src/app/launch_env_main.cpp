// Standalone `--print-launch-env` / `--print-launch-config` tool for the asset-free tests: the same
// fukami::settings::launchEnvMain the product binary runs, without the game.
#include "fukami_settings.h"

int main(int argc, char **argv)
{
    return fukami::settings::launchEnvMain(argc, argv);
}
