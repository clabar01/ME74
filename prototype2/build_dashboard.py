import re
t = open('src/dashboard.template.html').read()
css = open('node_modules/leaflet/dist/leaflet.css').read()
js = open('node_modules/leaflet/dist/leaflet.js').read()
js = re.sub(r'//# sourceMappingURL=.*', '', js)
assert '</script' not in js.lower() and '</style' not in css.lower()
out = t.replace('/*LEAFLET_CSS*/', css, 1).replace('/*LEAFLET_JS*/', js, 1)
open('ME74_P2_position_dashboard.html', 'w').write(out)
print(len(out), 'bytes; em dashes in template:', t.count('—'))
