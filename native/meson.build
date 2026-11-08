project('air-native', 'c', default_options: ['c_std=c11','warning_level=2'])
cc=meson.get_compiler('c')
protos=dependency('wayland-protocols').get_variable('pkgdatadir')
scanner=find_program('wayland-scanner')
layer=get_option('layer_protocol')
xmls=[protos/'stable/xdg-shell/xdg-shell.xml',layer,
 protos/'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml',
 protos/'staging/ext-image-capture-source/ext-image-capture-source-v1.xml',
 protos/'staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml',
 protos/'stable/presentation-time/presentation-time.xml']
generated=[]
foreach xml:xmls
 base=xml.split('/')[-1].replace('.xml','')
 generated += custom_target(base+'-header',input:xml,output:base+'-protocol.h',command:[scanner,'client-header','@INPUT@','@OUTPUT@'])
 generated += custom_target(base+'-code',input:xml,output:base+'-protocol.c',command:[scanner,'private-code','@INPUT@','@OUTPUT@'])
endforeach
executable('air-presenter','presenter/presenter.c',generated,include_directories:include_directories('common'),dependencies:[dependency('wayland-client'),dependency('wayland-egl'),dependency('egl'),dependency('glesv2'),dependency('cairo'),cc.find_library('m')])
math_test=executable('air-math-test','tests/math.c',include_directories:include_directories('common'),dependencies:cc.find_library('m'))
test('spatial geometry',math_test)
