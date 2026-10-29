import argparse
import subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description="Build the actual sanoTTS host reference and text contracts")
parser.add_argument('--output',type=Path,default=root/'build/host-tts')
args=parser.parse_args()
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
core=root/'components/sanotts';main=root/'main'
objects=[]
for name in ['saanotts','saanotts_stream','saanotts_int8','fft','g2p','saan_pcm']:
 obj=out/(name+'.o');objects.append(str(obj))
 subprocess.run(['clang','-O2','-std=c99','-DSAAN_INT8_ACT=1','-DSAAN_PIE_EMU=1','-I'+str(core),'-c',str(core/(name+'.c')),'-o',str(obj)],check=True)
for source,binary,used in [('tts_reference','tts_reference',objects),('test_tts_text','test_tts_text',[str(out/'g2p.o')])]:
 subprocess.run(['clang++','-O2','-std=c++17','-I'+str(core),'-I'+str(main),str(root/'scripts'/(source+'.cpp')),str(main/'tts_text.cpp'),*used,'-lm','-o',str(out/binary)],check=True)
subprocess.run([str(out/'test_tts_text')],check=True)
