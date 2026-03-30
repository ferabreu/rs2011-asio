#pragma once

void InitPatcher();
void DeinitPatcher();
void PatchOriginalCode();
void Patch_ReplaceWithNops(void* offset, size_t numBytes);
void Patch_ReplaceWithBytes(void* offset, size_t numBytes, const BYTE* replaceBytes);
