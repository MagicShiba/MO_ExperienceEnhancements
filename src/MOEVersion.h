/* MOEVersion.h - 版本号 (独立叶子头, 只被需要它的 .cpp 直接包含)。
 *
 * 单独成文件的原因: 版本号在每个改动都会 +1; 若放在被广泛包含的 MOExperience.h 里,
 * 每次提版本都会触发一批无关文件重编 (build-fast.bat 按 .cpp 真实包含的头判定).
 * 现在只有 MOExperience.cpp / SyringeHooks.cpp 直接包含本头, 提版本只重编这 2 个。 */
#pragma once

#define MOE_VERSION "0.2.344"
