# Stage 0 spikes (spec section 9). Each spike is a mini-project.
# Targets are added by separate CMakeLists.txt files as plan tasks are implemented:
#   common/  — Record.h, TraySpike.h (task 2)
#   s1/hook/ — TrayMirrorSpikeHook.dll (task 3)
#   s1/ctl/  — TraySpikeCtl.exe (task 4)
#   s1/emu/  — TrayIconEmu.exe (task 5)
#   s1/dump/ — TraySpikeDump.exe (task 7)

add_subdirectory(common)
add_subdirectory(s1)
