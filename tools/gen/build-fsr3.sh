#!/bin/sh
# AMD FSR 3.1's upscaler passes (lib/FidelityFX-SDK, v1.1.4: the Vulkan backend's GLSL) as one metallib for the Metal bundle's
# MetalFX temporal scaler (docs/fsr3-metalfx-plan.md): glslang -> SPIR-V, SPIRV-Cross -> MSL, Apple's metal -> AIR, with
# tools/metal/native/fsr3.metal (the scaler's clears). Our device translates the functions at first use, like any app's.
#
#   tools/gen/build-fsr3.sh <out.metallib>
#
# Functions: fsr3_<pass>_<h><d>, h = HDR colour input, d = inverted depth (MetalFX's depthReversed), the permutations MetalFX's
# settings ask for; the rest as AMD's defaults for this case: motion vectors at input resolution and without jitter, the reference
# Lanczos, 32-bit maths (FFX_HALF 0), no sharpening (accumulate writes the output).
# Needs glslangValidator and spirv-cross (Homebrew: glslang, spirv-cross).
set -eu
cd "$(dirname "$0")/../.."
out=$1
sdk=lib/FidelityFX-SDK/sdk
src=$sdk/src/backends/vk/shaders/fsr3upscaler
gpu=$sdk/include/FidelityFX/gpu
b=Build/fsr3
[ -f "$src/ffx_fsr3upscaler_accumulate_pass.glsl" ] || { echo "build-fsr3.sh: $src missing (git submodule update --init lib/FidelityFX-SDK)" >&2; exit 1; }
mkdir -p "$b"
passes="prepare_inputs luma_pyramid shading_change_pyramid shading_change prepare_reactivity luma_instability accumulate"
airs=""
for h in 0 1; do
	for d in 0 1; do
		for p in $passes; do
			n=fsr3_${p}_$h$d
			glslangValidator -V -q --target-env vulkan1.2 -S comp -e main -DFFX_GPU=1 -DFFX_GLSL=1 -DFFX_HALF=0 \
				-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0 -DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0 \
				-DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=0 -DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0 \
				-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2 -DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE=0 \
				-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=$h -DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1 \
				-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0 -DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=$d \
				-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=0 -I"$gpu" -I"$gpu/fsr3upscaler" \
				"$src/ffx_fsr3upscaler_${p}_pass.glsl" -o "$b/$n.spv" > "$b/$n.log" 2>&1 || { cat "$b/$n.log" >&2; exit 1; }
			spirv-cross "$b/$n.spv" --msl --msl-version 30000 --rename-entry-point main "$n" comp --output "$b/$n.metal"
			# SPIRV-Cross emulates 2D texture atomics with a buffer of rows aligned to this (a function constant by default, which
			# the translator cannot take): the scaler makes those textures from buffers with 256-byte rows, our device's alignment
			sed -i '' 's/^constant uint spvLinearTextureAlignment = .*/constant uint spvLinearTextureAlignment = 256;/' "$b/$n.metal"
			xcrun -sdk macosx metal -c -target air64-apple-macos13 -w "$b/$n.metal" -o "$b/$n.air"
			airs="$airs $b/$n.air"
		done
	done
done
xcrun -sdk macosx metal -c -target air64-apple-macos13 tools/metal/native/fsr3.metal -o "$b/fsr3_clear.air"
# shellcheck disable=SC2086
xcrun -sdk macosx metallib $airs "$b/fsr3_clear.air" -o "$out"
