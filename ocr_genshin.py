"""OCR all images from GenshinImpact.sp.exe成品项目界面UI folder."""
import easyocr
import json
import os
import numpy as np

reader = easyocr.Reader(['ch_sim', 'en'], gpu=True)

img_dir = r"d:\EXE\Ai\TRAE SOLO CN\Jinn.Projects\aimbot\mybot-main\GenshinImpact.sp.exe成品项目界面UI"

# Use cv2.imdecode to handle Unicode paths on Windows
import cv2

results = {}
for i in range(1, 10):
    fname = f"{i}.png"
    fpath = os.path.join(img_dir, fname)
    if not os.path.exists(fpath):
        print(f"  SKIP: {fname} not found")
        continue
    print(f"Processing {fname} ...")
    # Read file as binary and decode with imdecode (handles Unicode paths)
    img_data = np.fromfile(fpath, dtype=np.uint8)
    img = cv2.imdecode(img_data, cv2.IMREAD_COLOR)
    if img is None:
        print(f"  ERROR: cannot decode {fname}")
        continue
    result = reader.readtext(img, detail=1, paragraph=False)
    lines = []
    for bbox, text, conf in result:
        lines.append({"text": text, "confidence": round(conf, 4), "bbox": [[int(p[0]), int(p[1])] for p in bbox]})
    results[fname] = lines
    print(f"  Found {len(lines)} text regions")

out_path = os.path.join(img_dir, "ocr_results.json")
with open(out_path, "w", encoding="utf-8") as f:
    json.dump(results, f, ensure_ascii=False, indent=2)
print(f"\nSaved to {out_path}")