#!/usr/bin/env python3
"""Aggregate completed direct-NNUE profile JSONL files."""
import json, statistics, pathlib

FILES = [
 'build/profile/checkpoint4-scalar-depth.jsonl',
 'build/profile/checkpoint4-classical-depth.jsonl',
 'build/profile/refresh-split-avx2-depth4-6.jsonl',
 'build/profile/decoupled-refresh-1t-depth8.jsonl',
 'build/profile/decoupled-refresh-2-4-8t-depth8.jsonl',
 'build/profile/checkpoint4-avx2-nodes10k.jsonl',
 'build/profile/checkpoint4-scalar-nodes10k.jsonl',
 'build/profile/checkpoint4-classical-nodes10k.jsonl',
]

def quartiles(v):
    v=sorted(v); n=len(v)
    if n < 2: return (v[0], v[0], 0.0)
    q1=statistics.median(v[:n//2]); q3=statistics.median(v[(n+1)//2:])
    return statistics.median(v), q1, q3-q1

rows=[]
for name in FILES:
    backend=None
    raw = pathlib.Path(name).read_bytes()
    encoding = 'utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf8'
    for line in raw.decode(encoding).splitlines():
        if line.startswith('profile_metadata='):
            backend=json.loads(line.removeprefix('profile_metadata='))['backend']
            continue
        try: item=json.loads(line)
        except json.JSONDecodeError: continue
        if 'profile_metadata' in item: backend=item['profile_metadata']['backend']
        elif 'backend' in item and 'compiler' in item: backend=item['backend']
        elif 'nps' in item:
            rows.append((backend, item['depth'], item['threads'], item['node_budget'], item))

groups={}
for backend, depth, threads, budget, row in rows:
    key=(backend, 'nodes' if budget else 'depth', budget or depth, threads)
    groups.setdefault(key, []).append(row)
summary=[]
for key, samples in sorted(groups.items()):
    nps,q1,iqr=quartiles([x['nps'] for x in samples])
    elapsed,_,_=quartiles([x['elapsed_ms'] for x in samples])
    nodes,_,_=quartiles([x['nodes'] for x in samples])
    summary.append({'evaluator':key[0], 'limit_type':key[1], 'limit':key[2], 'threads':key[3],
                    'samples':len(samples), 'median_nps':nps, 'iqr_nps':iqr,
                    'median_elapsed_ms':elapsed, 'median_nodes':nodes,
                    'completed_depth': key[2] if key[1]=='depth' else None,
                    'qnodes':'not emitted by completed profile harness',
                    'evaluations_per_node':'not emitted by completed profile harness'})
lookup={(x['evaluator'],x['limit_type'],x['limit'],x['threads']):x for x in summary}
for x in summary:
    base=lookup.get((x['evaluator'],x['limit_type'],x['limit'],1))
    x['thread_scaling_efficiency']=None if not base else x['median_nps']/(base['median_nps']*x['threads'])
    scalar=lookup.get(('scalar',x['limit_type'],x['limit'],x['threads']))
    avx=lookup.get(('avx2',x['limit_type'],x['limit'],x['threads']))
    classical=lookup.get(('classical',x['limit_type'],x['limit'],x['threads']))
    if avx and scalar: x['scalar_to_avx2_speedup']=avx['median_nps']/scalar['median_nps']
    if classical and x['evaluator'] in ('scalar','avx2'): x['direct_to_classical_ratio']=x['median_nps']/classical['median_nps']
strong=max(summary,key=lambda x:x['median_nps']); weak=min(summary,key=lambda x:x['median_nps'])
out={'source_files':FILES,'configurations':summary,'strongest':strong,'weakest':weak,
     'notes':['Completed search JSONL lacks qnode counts and evaluation counts; these are reported only by component profiles.',
              'AVX2 depth-8 samples are from the completed 54980c7 retained build.']}
pathlib.Path('build/profile/checkpoint4-matrix-summary.json').write_text(json.dumps(out,indent=2)+'\n',encoding='utf8')
lines=['# Completed NNUE benchmark matrix','','Source: completed 8-position, five-run profile matrices. IQR is Q3 minus Q1.','',
       '| Evaluator | Limit | Threads | Median NPS | IQR NPS | Median ms | Median nodes | Scaling efficiency | Scalar→AVX2 | Direct/classical |',
       '|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|']
for x in summary:
    f=lambda v: '—' if v is None else f'{v:.3f}'
    lines.append(f"| {x['evaluator']} | {x['limit_type']} {x['limit']} | {x['threads']} | {x['median_nps']:.1f} | {x['iqr_nps']:.1f} | {x['median_elapsed_ms']:.1f} | {x['median_nodes']:.0f} | {f(x['thread_scaling_efficiency'])} | {f(x.get('scalar_to_avx2_speedup'))} | {f(x.get('direct_to_classical_ratio'))} |")
lines += ['',f"Strongest: **{strong['evaluator']} {strong['limit_type']} {strong['limit']} at {strong['threads']}T** ({strong['median_nps']:.1f} NPS).",f"Weakest: **{weak['evaluator']} {weak['limit_type']} {weak['limit']} at {weak['threads']}T** ({weak['median_nps']:.1f} NPS).",'',
          '## Availability notes','Completed depth is the fixed depth limit for depth runs. Qnodes, evaluation-per-node, allocations, FEN conversions, and Stockfish-position construction counts were not emitted by the completed general-search profile JSONL, so they cannot be reconstructed honestly from those files. The current component profile separately reports 0.6726 evaluations/node and 83.42% qsearch for its depth-8 AVX2 workload.']
pathlib.Path('build/profile/checkpoint4-matrix-summary.md').write_text('\n'.join(lines)+'\n',encoding='utf8')
