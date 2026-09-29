"""Compare original, camera-only and camera+pass output on an owned capture, headlessly."""
import hashlib
import json
import math
import os
import struct
import sys

runtime, capture, config_path, recipe_path, output = sys.argv[1:]
sys.path.insert(0, os.path.join(runtime, 'pymodules'))
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '../../../qriderduck/Resources')))
import riderduck as rd
from freecamera_common import inverse, multiply, transpose

with open(config_path) as f: doc = json.load(f)
config = doc.get('configuration', doc.get('config', doc))
with open(recipe_path) as f: recipe = json.load(f)
os.makedirs(output, exist_ok=True)
rd.InitialiseReplay(rd.GlobalEnvironment(), [])
cap = rd.OpenCaptureFile()
assert cap.OpenFile(capture, '', None) == rd.ResultCode.Succeeded
status, r = cap.OpenCapture(rd.ReplayOptions(), None)
assert status == rd.ResultCode.Succeeded
try:
    ids = {str(x.resourceId): rd.ResourceId(x.resourceId) for x in r.GetResources()}
    stages = dict(VS=rd.ShaderStage.Vertex,HS=rd.ShaderStage.Hull,DS=rd.ShaderStage.Domain,
                  GS=rd.ShaderStage.Geometry,PS=rd.ShaderStage.Pixel,CS=rd.ShaderStage.Compute)
    event = config.get('event_id',doc.get('event_id',7813))
    output_id = ids[config['output_rid']]
    pose = inverse(config['view'])
    a=.055
    rotation=[math.cos(a),0,math.sin(a),0, 0,1,0,0, -math.sin(a),0,math.cos(a),0, 0,0,0,1]
    moved=multiply(rotation,pose)
    for i in (3,7,11): moved[i]=pose[i]
    view=inverse(moved)
    patches=[]
    for field in config['fields']:
        p=rd.ReplayConstantOverride();p.shader=ids[field['shader']];p.stage=stages[field['stage']]
        p.slot=field['slot'];p.byteOffset=field['offset']
        p.firstEvent=field['first_event'];p.lastEvent=field['last_event']
        projection=field.get('projection',config['projection'])
        relative=list(view);relative[3]=relative[7]=relative[11]=0
        semantic=field['semantic']
        if semantic=='CameraPosition': values=[moved[i] for i in (3,7,11)]
        else:
            values={'View':lambda:view,'InverseView':lambda:moved,'Projection':lambda:projection,
                    'InverseProjection':lambda:inverse(projection),'ViewProjection':lambda:multiply(projection,view),
                    'InverseViewProjection':lambda:inverse(multiply(projection,view)),
                    'RelativeViewProjection':lambda:multiply(projection,relative),
                    'InverseRelativeViewProjection':lambda:inverse(multiply(projection,relative)),
                    'ModelView':lambda:multiply(view,field['model'])}[semantic]()
            if field.get('row_vector',False): values=transpose(values)
            if not field.get('row_major',True): values=transpose(values)
        p.data=struct.pack('<{}f'.format(len(values)),*values);patches.append(p)
    passes=[]
    for rule in recipe['passes']:
        p=rd.ReplayPassOverride();p.firstEvent=rule['first_event'];p.lastEvent=rule['last_event']
        p.bypass=rule['mode']=='colour_bypass'
        colours=[]
        for entry in rule.get('colours',[]):
            c=rd.ReplayColourMapping();c.source=ids[entry['source']];c.destination=ids[entry['destination']]
            for direction in ('source','destination'):
                for part in ('mip','slice','format'):
                    setattr(c,direction+part.title(),entry.get(direction+'_'+part,0))
            colours.append(c)
        p.colours=colours;passes.append(p)
    results=dict(event_id=event,view=view,passes=recipe['passes'],images={},auxiliary_uses={})
    for name,constants,rules in [('original',[],[]),('camera-only',patches,[]),('camera-bypass',patches,passes),('restored',[],[])]:
        error=r.SetReplayOverrides(constants,rules,event)
        assert not error,error
        data=bytes(r.GetTextureData(output_id,rd.Subresource()))
        save=rd.TextureSave();save.resourceId=output_id;save.destType=rd.FileType.PNG
        assert r.SaveTexture(save,os.path.join(output,name+'.png'))==rd.ResultCode.Succeeded
        results['images'][name]=dict(sha256=hashlib.sha256(data).hexdigest(),bytes=len(data))
        if name in ('camera-only','camera-bypass'):
            for label,rid,eid in [('pre-temporal','ResourceId::1842',4799),('post-temporal','ResourceId::153044',4811)]:
                r.SetFrameEvent(eid,True)
                save.resourceId=ids[rid]
                assert r.SaveTexture(save,os.path.join(output,name+'-'+label+'.png'))==rd.ResultCode.Succeeded
            r.SetFrameEvent(event,True)
    # Auxiliary outputs of 4799 must have no downstream readers after the bypass endpoint.
    for rid in ('ResourceId::1769','ResourceId::153048','ResourceId::1785'):
        results['auxiliary_uses'][rid]=[dict(eid=u.eventId,usage=int(u.usage)) for u in r.GetUsage(ids[rid]) if u.eventId>4811]
    results['reset_exact']=results['images']['original']['sha256']==results['images']['restored']['sha256']
    with open(os.path.join(output,'comparison.json'),'w') as f: json.dump(results,f,indent=2)
    print(json.dumps(results,indent=2))
finally:
    r.Shutdown();cap.Shutdown();rd.ShutdownReplay()
