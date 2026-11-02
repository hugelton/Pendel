// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
//
// Pocket AMP .fwsc packages in the browser (after Felucca's fm1pkg.js):
//   productOf, logicalImage  the package identity and the image the device reads during an update
//   validateOfficial         the official file from M-VAVE, for the way back

const BLOCKS = 20, BLK = 0x30, KEEP = 0x2F;
export const OFFICIAL_SHA256 = "9584e81da98215e8e70ddffc28311423ceaa1a5a8a12a478dc68f4679e10902b";
export const OFFICIAL_SIZE = 767028;
export const OFFICIAL_PRODUCT = "Pocket AMP_105";

// the package identity ("Pocket AMP_910"): one marker byte after each of the first 20 blocks
export function productOf(fwsc) {
  if (fwsc.length < BLOCKS * BLK) throw new Error("not a Pocket AMP package (too short)");
  let s = "";
  for (let i = 0; i < BLOCKS; i++) {
    const m = fwsc[i * BLK + KEEP];
    if (m !== 0x7D) s += String.fromCharCode((m - i - 1) & 0xFF);
  }
  return s;
}

// the image the device addresses during an update: the .fwsc without the 20 marker bytes
export function logicalImage(fwsc) {
  const out = new Uint8Array(fwsc.length - BLOCKS);
  for (let i = 0; i < BLOCKS; i++) out.set(fwsc.subarray(i * BLK, i * BLK + KEEP), i * KEEP);
  out.set(fwsc.subarray(BLOCKS * BLK), BLOCKS * KEEP);
  return out;
}

export async function sha256hex(bytes) {
  const d = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(d)].map((x) => x.toString(16).padStart(2, "0")).join("");
}

// Only the unmodified official file is accepted for the way back; the digest pins all of it.
export async function validateOfficial(bytes) {
  if (bytes.length !== OFFICIAL_SIZE || await sha256hex(bytes) !== OFFICIAL_SHA256 || productOf(bytes) !== OFFICIAL_PRODUCT)
    throw new Error("Select the unmodified official Pocket AMP file (Pocket AMP.fwsc).");
  return { product: OFFICIAL_PRODUCT, image: logicalImage(bytes) };
}
