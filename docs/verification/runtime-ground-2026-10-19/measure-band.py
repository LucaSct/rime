# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 The Rime Engine Authors.
from pathlib import Path
from PIL import Image
import numpy as np,json
p=Path(__file__).parent;result={}
for name in ['floor-band-before.png','floor-band-after.png','floor-ssr-off-control.png','floor-ssr-off-after.png']:
 image=np.asarray(Image.open(p/name)).astype(float)/255
 luma=image@np.array([.2126,.7152,.0722]);profile=luma[360:390,320:960].mean(axis=1);steps=np.diff(profile);i=int(np.argmax(abs(steps)))
 result[name]={'max_mean_row_step':float(abs(steps[i])),'row':360+i,'mean_above':float(profile[i]),'mean_below':float(profile[i+1])}
print(json.dumps(result,indent=2))
