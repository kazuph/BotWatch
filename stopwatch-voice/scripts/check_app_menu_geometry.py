#!/usr/bin/env python3
"""Exhaustively check every native circular menu rotation and every button pair."""
import json,math,re,struct
from pathlib import Path
root=Path(__file__).resolve().parents[1];source=(root/'main/voice_screen.cpp').read_text();section=source[source.index('constexpr const char *app_names'):source.index('void app_position')]
labels=re.findall(r'\{"([^"]*)", "([^"]*)"\}',section);radius,button=map(int,re.search(r'app_radius=(\d+), app_button_radius=(\d+)',section).groups())
rows=[]
for rotation in range(len(labels)):
 centers=[(233+round(radius*math.cos((i-rotation)*2*math.pi/len(labels)-math.pi/2)),233+round(radius*math.sin((i-rotation)*2*math.pi/len(labels)-math.pi/2))) for i in range(len(labels))]
 for i,(x,y) in enumerate(centers):
  assert math.hypot(x-233,y-233)+button<233
  for xx,yy in centers[i+1:]:assert math.hypot(x-xx,y-yy)>2*button
 rows.append({'rotation':rotation,'centers':centers})
print(json.dumps({'buttons':len(labels),'rotations':len(rows),'overlaps':0,'outside_circle':0,'positions':rows}))
