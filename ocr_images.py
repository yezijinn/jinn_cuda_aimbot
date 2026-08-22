import easyocr
import os
import json
import sys
from PIL import Image
import numpy as np

sys.stdout.reconfigure(encoding='utf-8')

img_dir = r"d:\EXE\Ai\TRAE SOLO CN\Jinn.Projects\aimbot\mybot-main\别人的成品项目界面UI"
output_file = os.path.join(os.path.dirname(img_dir), "ocr_results.json")

reader = easyocr.Reader(['ch_sim', 'en'], gpu=True)

images = sorted([f for f in os.listdir(img_dir) if f.endswith('.png')])
results = {}

for img_name in images:
    img_path = os.path.join(img_dir, img_name)
    print(f"\n{'='*60}")
    print(f"Processing: {img_name}")
    print('='*60)
    
    try:
        pil_img = Image.open(img_path).convert('RGB')
        img_array = np.array(pil_img)
        print(f"  Image size: {pil_img.size}")
        
        ocr_result = reader.readtext(img_array, detail=1)
        texts = []
        for bbox, text, conf in ocr_result:
            y_center = (bbox[0][1] + bbox[2][1]) / 2
            texts.append({
                "text": text,
                "confidence": round(conf, 3),
                "y_center": int(y_center)
            })
            print(f"  [{conf:.3f}] {text}")
        
        results[img_name] = texts
    except Exception as e:
        print(f"  ERROR: {e}")
        results[img_name] = []

with open(output_file, 'w', encoding='utf-8') as f:
    json.dump(results, f, ensure_ascii=False, indent=2)

print(f"\nResults saved to {output_file}")
print(f"Total images: {len(results)}")