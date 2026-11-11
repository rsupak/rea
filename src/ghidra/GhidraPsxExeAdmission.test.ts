import { createHash } from "node:crypto";
import { readFile, writeFile } from "node:fs/promises";
import { isAbsolute, join, relative, resolve } from "node:path";

import { describe, expect, it } from "vitest";

import { parseBinaryTarget } from "../application/BinaryTargetResolver.js";
import { parseConfig } from "../config/parseConfig.js";
import { convertPsxExeToElf32Mips } from "../domain/psxExe.js";
import { silentLogger } from "../logger.js";
import { createTestTempDirectory } from "../../tests/fixtures/temporaryDirectory.js";
import { GhidraProvider } from "./GhidraProvider.js";
import { ghidraProfileInstallationHost as host } from "./GhidraInstallation.fixture.js";
import {
  ghidraMips1Limitations,
  isMips1LegacyAdmission,
} from "./GhidraMipsProfile.js";

const parsed = parseConfig({ GHIDRA_INSTALL_DIR: "/ghidra" });
if (!parsed.ok) throw parsed.error;
const config = parsed.value;

/** Hand-authored payload. The entry is 16 bytes after the load address. */
const syntheticExe = (): Buffer => {
  const body = Buffer.alloc(2048);
  body.writeUInt32LE(0x27bd_fff0, 16);
  body.writeUInt32LE(0x03e0_0008, 20);
  body.writeUInt32LE(0x27bd_0010, 24);
  const file = Buffer.alloc(2048 + body.length);
  file.write("PS-X EXE", 0, "ascii");
  file.writeUInt32LE(0x8001_0010, 0x10);
  file.writeUInt32LE(0x8001_0000, 0x18);
  file.writeUInt32LE(body.length, 0x1c);
  file.writeUInt32LE(0x801f_fff0, 0x30);
  file.write("synthetic", 0x4c, "ascii");
  body.copy(file, 2048);
  return file;
};

const admitElf = async (bytes: Buffer, name: string) => {
  const directory = await createTestTempDirectory("rea-psx-elf-");
  const path = join(directory, name);
  await writeFile(path, bytes);
  const target = await parseBinaryTarget(path);
  if (!target.ok) throw target.error;
  return target.value;
};

describe("PS-X EXE ELF admission", () => {
  it("admits a synthetic image through the recordless MIPS-I lane", async () => {
    const converted = convertPsxExeToElf32Mips(syntheticExe());
    expect(converted.ok).toBe(true);
    if (!converted.ok) return;
    const target = await admitElf(converted.value.elf, "synthetic.elf");
    expect(target).toMatchObject({
      kind: "executable",
      format: "elf",
      architecture: "mips",
      mips: {
        elfClass: 32,
        byteOrder: "little",
        type: 2,
        flags: 0,
        abiFlags: null,
        gnuAttributes: null,
      },
    });
    if (target.architecture !== "mips")
      throw new Error("expected a MIPS target");
    expect(target.mips.limitations).toBeUndefined();
    expect(isMips1LegacyAdmission(target)).toBe(true);
    expect(ghidraMips1Limitations(target, undefined, true).join(" ")).toContain(
      "MIPS32",
    );
    const ghidra = new GhidraProvider(config, silentLogger, {}, host);
    expect(ghidra.inspectTargetSupport(target)).toMatchObject({
      status: "supported",
      code: null,
    });
    const resolved = await ghidra.resolveAnalysisProfile(target);
    if (!resolved.ok) throw resolved.error;
    expect(resolved.value.profile?.parameters).toMatchObject({
      mips_support_lane: "elf32-exec-mips1-legacy-pinned-v1",
      mips_abi_basis: "inferred-o32-assumption-v1",
      language_id: "MIPS:LE:32:default",
      compiler_spec_id: "default",
      language_selection: "provider-pinned-v1",
    });
  });

  const supplied = process.env.REA_PSX_EXE;
  it.skipIf(supplied === undefined)(
    "admits a caller-supplied PS-X EXE without recording its bytes",
    async () => {
      if (supplied === undefined) throw new Error("REA_PSX_EXE is unset");
      const bytes = await readFile(supplied);
      const sha1 = createHash("sha1").update(bytes).digest("hex");
      if (process.env.REA_PSX_EXE_SHA1 !== undefined)
        expect(sha1).toBe(process.env.REA_PSX_EXE_SHA1);
      const converted = convertPsxExeToElf32Mips(bytes);
      expect(converted.ok).toBe(true);
      if (!converted.ok) throw new Error(converted.error);
      const { facts, elf } = converted.value;
      const hex = (value: number): string => `0x${value.toString(16)}`;
      if (process.env.REA_PSX_EXPECT_PC !== undefined)
        expect(hex(facts.initialPc)).toBe(process.env.REA_PSX_EXPECT_PC);
      if (process.env.REA_PSX_EXPECT_LOAD !== undefined)
        expect(hex(facts.loadAddress)).toBe(process.env.REA_PSX_EXPECT_LOAD);
      if (process.env.REA_PSX_EXPECT_SP !== undefined)
        expect(hex(facts.stackPointer)).toBe(process.env.REA_PSX_EXPECT_SP);
      if (process.env.REA_PSX_EXPECT_SIZE !== undefined)
        expect(facts.payloadSize).toBe(Number(process.env.REA_PSX_EXPECT_SIZE));
      if (process.env.REA_PSX_EXPECT_MARKER !== undefined)
        expect(facts.regionMarker).toBe(process.env.REA_PSX_EXPECT_MARKER);
      const output = process.env.REA_PSX_ELF_OUT;
      if (output !== undefined) {
        const absolute = resolve(output);
        const insideRepository =
          relative(resolve("."), absolute) === "" ||
          (!relative(resolve("."), absolute).startsWith("..") &&
            !isAbsolute(relative(resolve("."), absolute)));
        if (insideRepository)
          throw new Error(
            "refusing to write a caller-supplied ELF inside the repository",
          );
        await writeFile(absolute, elf);
      }
      const target = await admitElf(elf, "supplied.elf");
      expect(isMips1LegacyAdmission(target)).toBe(true);
      const provider = new GhidraProvider(config, silentLogger, {}, host);
      expect(provider.inspectTargetSupport(target).status).toBe("supported");
      const resolved = await provider.resolveAnalysisProfile(target);
      if (!resolved.ok) throw resolved.error;
      expect(resolved.value.profile?.parameters).toMatchObject({
        mips_support_lane: "elf32-exec-mips1-legacy-pinned-v1",
        language_id: "MIPS:LE:32:default",
        language_selection: "provider-pinned-v1",
      });
    },
  );
});
