#!/usr/bin/env python3
"""
Auto-generate PE module I/O HTML documentation.
Scans include/hybridacc/pe/*.hpp and extracts sc_in / sc_out port declarations.
Produces docs/pe_components.html with per-module SVG diagrams grouped by functional categories.
Heuristics assign each port to a category (Clock/Reset, Memory IF, Config, Control, Data, RV-Channel, etc.).
Usage:
  python scripts/gen_pe_io_html.py
"""
import re, pathlib, datetime, json

ROOT = pathlib.Path(__file__).resolve().parents[1]  # hybridacc-ESL
INC_DIR = ROOT / 'include' / 'hybridacc' / 'pe'
OUT_HTML = ROOT / 'docs' / 'pe_components.html'

PORT_RE = re.compile(r'sc_(in|out)\s*<([^>]+)>\s+([^;]+);')
SC_MODULE_RE = re.compile(r'SC_MODULE\(([^)]+)\)')
CLASS_MODULE_RE = re.compile(r'class\s+([A-Za-z0-9_]+)\s*:\s*public\s+sc_core::sc_module')

CATEGORY_RULES = [
    ('Clock/Reset', lambda n,t,d: n in ('clk','rst','rst_n','reset','areset_n')),
    ('Data Memory IF', lambda n,t,d: n.startswith('dm_')),
    ('Inst Memory IF', lambda n,t,d: n.startswith('im_')),
    ('Static Port', lambda n,t,d: n.startswith('ps_')),
    ('Port Local In', lambda n,t,d: n.startswith('pli_')),
    ('Port Local Out', lambda n,t,d: n.startswith('plo_')),
    ('Dynamic Port', lambda n,t,d: n.startswith('pd_')),
    ('Loop Control', lambda n,t,d: 'loop' in n or 'jump' in n or 'lpstk' in n),
    ('Config', lambda n,t,d: any(k in n for k in ('addr','len','mode','stride','cfg','mask'))),
    ('Control', lambda n,t,d: any(k in n for k in ('set_','activate','next','push','pop','en_i','wen','shift_en','clear','start'))),
    ('Status', lambda n,t,d: any(k in n for k in ('done','busy','empty'))),
    ('Data Out', lambda n,t,d: n.endswith('_o') and any(k in n for k in ('data','vec','out','p_o'))),
    ('Data In', lambda n,t,d: n.endswith('_i') and any(k in n for k in ('data','vec','p_i'))),
]

def categorize(name, tp, direction):
    for cat, pred in CATEGORY_RULES:
        if pred(name, tp, direction):
            return cat
    return 'Misc'

PORT_LINE_START = re.compile(r'\s*sc_(in|out)\s*<')

def collect_statements(path_text:str):
    stmts=[]; buf=[]
    for line in path_text.splitlines():
        buf.append(line)
        if ';' in line:
            stmt='\n'.join(buf)
            stmts.append(stmt)
            buf=[]
    if buf: stmts.append('\n'.join(buf))
    return stmts

def parse_ports_from_statement(stmt:str):
    m = re.search(r'sc_(in|out)\s*<', stmt)
    if not m: return []
    direction = m.group(1)
    start = m.end()-1
    depth=0; type_chars=[]; i=start
    while i < len(stmt):
        ch = stmt[i]
        if ch == '<':
            depth += 1
            if depth>1: type_chars.append(ch)
        elif ch == '>':
            depth -=1
            if depth==0:
                i+=1
                break
            else:
                type_chars.append(ch)
        else:
            if depth>=1: type_chars.append(ch)
        i+=1
    type_str=''.join(type_chars).strip()
    rest = stmt[i:]
    semi = rest.find(';')
    if semi!=-1:
        rest = rest[:semi]
    rest = rest.replace('\n',' ')
    parts=[]; cur=''; brace=0
    for ch in rest:
        if ch=='{' or ch=='(':
            brace+=1
        elif ch=='}' or ch==')':
            if brace>0: brace-=1
        if ch==',' and brace==0:
            parts.append(cur)
            cur=''
        else:
            cur+=ch
    if cur.strip(): parts.append(cur)
    ports=[]
    for p in parts:
        p=p.strip()
        if not p: continue
        nm_match = re.match(r'([A-Za-z0-9_]+)', p)
        if not nm_match: continue
        name = nm_match.group(1)
        cat = categorize(name, type_str, direction)
        width_expr=''
        width_bits=None
        m_w = re.match(r'\s*sc_uint\s*<\s*([^>]+)\s*>', type_str)
        if m_w:
            expr = m_w.group(1).strip()
            width_expr=expr
            if expr.isdigit():
                width_bits=int(expr)
        elif type_str.strip()=='bool':
            width_bits=1
        label=name
        if width_bits and width_bits>1:
            label=f"{name}[{width_bits-1}:0]"
        elif width_expr and not width_expr.isdigit():
            label=f"{name}{{{width_expr}}}"
        ports.append({'name': name, 'label': label, 'type': type_str, 'dir': direction, 'category': cat, 'width': width_bits or width_expr or 1})
    return ports

def extract_ports(text):
    ports=[]
    for stmt in collect_statements(text):
        if 'sc_in<' in stmt or 'sc_out<' in stmt:
            ports.extend(parse_ports_from_statement(stmt))
    return ports

def find_modules(path: pathlib.Path):
    modules = []
    for f in sorted(path.glob('*.hpp')):
        txt = f.read_text(encoding='utf-8', errors='ignore')
        names = set(SC_MODULE_RE.findall(txt))
        class_names = CLASS_MODULE_RE.findall(txt)
        for n in class_names:
            names.add(n)
        for n in sorted(names):
            ports = extract_ports(txt)
            modules.append({'file': f.name, 'name': n, 'ports': ports})
    return modules

def build_dataset(mods):
    dataset = []
    for m in mods:
        inputs = [p for p in m['ports'] if p['dir']=='in']
        outputs= [p for p in m['ports'] if p['dir']=='out']
        inputs.sort(key=lambda p:(p['category'], p['name']))
        outputs.sort(key=lambda p:(p['category'], p['name']))
        dataset.append({'name': m['name'], 'file': m['file'], 'inputs': inputs, 'outputs': outputs})
    return dataset

def generate_html(dataset):
    json_text = json.dumps(dataset, ensure_ascii=False, indent=2)
    time_str = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    # 調整 SVG 版面：加寬水平空間 (padX 170->220, boxW 320->420, port 寬 150->200)
    return f"""<!DOCTYPE html><html lang=\"zh-TW\"><head><meta charset=\"UTF-8\" />
<title>PE 子模組 I/O 介面圖 (Auto-Generated)</title>
<style>
body {{ font-family: Arial, 'Noto Sans CJK', sans-serif; margin:16px; background:#fafafa; }}
h1 {{ margin-top:0; }}
.module {{ margin:32px 0 56px; }}
svg {{ background:#fff; border:1px solid #ccc; border-radius:8px; box-shadow:2px 2px 5px #ccc; }}
.port-in {{ fill:#e3f2fd; }}
.port-out {{ fill:#ffe0b2; }}
.port-cat {{ font-size:10px; fill:#555; }}
.module-box {{ fill:#fafafa; stroke:#444; stroke-width:1.1; }}
text {{ font-family: Arial, sans-serif; font-size:11px; }}
.label-header {{ font-weight:bold; font-size:13px; }}
footer {{ margin-top:40px; font-size:12px; color:#555; }}
</style></head><body>
<h1>HybridAcc PE 子模組 I/O 介面示意</h1>
<p style='font-size:13px;color:#444'>本頁由腳本 <code>scripts/gen_pe_io_html.py</code> 自動產生。每個模組左側列輸入、右側列輸出；依功能分類排序。Hover 以檢視型別。</p>
<hr/>
<script id=\"io_dataset\" type=\"application/json\">{json_text}</script>
<script>
function groupPorts(list){{const groups=[];const map={{}};list.forEach(p=>{{if(!map[p.category]){{map[p.category]=[];groups.push({{cat:p.category,items:map[p.category]}});}}map[p.category].push(p);}});return groups;}}
function countRows(groups){{let total=0;groups.forEach(g=>{{if(g.items.length){{total+=g.items.length; // actual port lines
 total+=1; // spacer after group
}}}});if(total>0) total-=1;return total;}}
function drawModule(m,idx){{
  const portInX=20, portInW=260; // input port box
  const gap=40;                  // gap between input port box and module box (與輸出 outOffset 一致)
  const padX=portInX+portInW+gap; // module box X (left edge)
  const padY=24, rowH=18, boxW=420; // keep module width
  const inG=groupPorts(m.inputs), outG=groupPorts(m.outputs);
  const rows=Math.max(countRows(inG), countRows(outG));
  const h=padY*2+rows*rowH+10;
  const w=padX+boxW + (boxW/2) + 260; // extend right side for outputs
  let svg=`<svg width='${{w}}' height='${{h}}' viewBox='0 0 ${{w}} ${{h}}'>`;
  const boxY=padY-16, boxH=rows*rowH+32;
  svg+=`<rect class='module-box' x='${{padX}}' y='${{boxY}}' width='${{boxW}}' height='${{boxH}}' rx='10'/>`;
  svg+=`<text x='${{padX+boxW/2}}' y='${{boxY+16}}' text-anchor='middle' class='label-header'>${{m.name}}</text>`;
  svg+=`<text x='${{padX+boxW/2}}' y='${{boxY+30}}' text-anchor='middle' font-size='10' fill='#666'>${{m.file}}</text>`;
  let inIdx=0;
  inG.forEach(g=>{{ if(g.items.length) svg+=`<text x='10' y='${{padY+rowH*inIdx-4}}' class='port-cat'>${{g.cat}}</text>`;
    g.items.forEach(p=>{{ const yy=padY+rowH*inIdx+10;
      svg+=`<rect class='port-in' x='${{portInX}}' y='${{yy-9}}' width='${{portInW}}' height='16' rx='4' title='${{p.type}}'/>`;
      svg+=`<text x='${{portInX+portInW/2}}' y='${{yy+3}}' text-anchor='middle'>${{p.label}}</text>`;
      svg+=`<line x1='${{portInX+portInW}}' y1='${{yy-1}}' x2='${{padX}}' y2='${{yy-1}}' stroke='#1976d2' stroke-width='1.2' marker-end='url(#ai_${{idx}})'/>`;
      inIdx++; }}); inIdx++; }});
  let outIdx=0;
  const outOffset=40; // distance from module box right edge to first output box
  const portOutW=260; const portOutX=padX+boxW+outOffset;
  outG.forEach(g=>{{ if(g.items.length) svg+=`<text x='${{portOutX}}' y='${{padY+rowH*outIdx-4}}' class='port-cat'>${{g.cat}}</text>`;
    g.items.forEach(p=>{{ const yy=padY+rowH*outIdx+10;
      svg+=`<rect class='port-out' x='${{portOutX}}' y='${{yy-9}}' width='${{portOutW}}' height='16' rx='4' title='${{p.type}}'/>`;
      svg+=`<text x='${{portOutX+portOutW/2}}' y='${{yy+3}}' text-anchor='middle'>${{p.label}}</text>`;
      svg+=`<line x1='${{padX+boxW}}' y1='${{yy-1}}' x2='${{portOutX}}' y2='${{yy-1}}' stroke='#ef6c00' stroke-width='1.2' marker-end='url(#ao_${{idx}})'/>`;
      outIdx++; }}); outIdx++; }});
  svg+=`<defs><marker id='ai_${{idx}}' viewBox='0 0 10 10' refX='10' refY='5' markerWidth='5' markerHeight='5' orient='auto'><path d='M0 0 L10 5 L0 10 Z' fill='#1976d2'/></marker><marker id='ao_${{idx}}' viewBox='0 0 10 10' refX='0' refY='5' markerWidth='5' markerHeight='5' orient='auto'><path d='M10 0 L0 5 L10 10 Z' fill='#ef6c00'/></marker></defs>`;
  svg+='</svg>';
  const wrap=document.createElement('div'); wrap.className='module'; wrap.innerHTML=`<h2>${{m.name}}</h2>`+svg; document.body.appendChild(wrap);
}}
const DATA=JSON.parse(document.getElementById('io_dataset').textContent);DATA.forEach((m,i)=>drawModule(m,i));
</script>
<footer>產生時間: {time_str} | 來源: include/hybridacc/pe/*.hpp | 自動分類僅供參考</footer></body></html>"""

if __name__ == '__main__':
    mods = find_modules(INC_DIR)
    dataset = build_dataset(mods)
    html_doc = generate_html(dataset)
    OUT_HTML.write_text(html_doc, encoding='utf-8')
    print(f"[gen_pe_io_html] Generated {OUT_HTML}")
