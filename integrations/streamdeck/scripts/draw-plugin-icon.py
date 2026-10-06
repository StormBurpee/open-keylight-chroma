from PIL import Image, ImageDraw
from pathlib import Path
for size,suffix in [(256,''),(512,'@2x')]:
 s=size/144
 im=Image.new('RGBA',(size,size),(0,0,0,0));d=ImageDraw.Draw(im)
 def line(points):d.line([(int(x*s),int(y*s)) for x,y in points],fill='#e9e5dc',width=max(1,int(5*s)),joint='curve')
 d.rounded_rectangle(tuple(int(v*s) for v in (29,24,115,90)),radius=int(12*s),outline='#e9e5dc',width=int(5*s))
 line([(72,90),(72,115)]);line([(51,117),(93,117)])
 for y in [39,53,67]:line([(43,y),(101,y)])
 im.save(Path('org.openkeylight.chroma.sdPlugin/imgs')/('plugin'+suffix+'.png'))
