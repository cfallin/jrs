import os

ccflags = '-O3 -g'

if False:
    # static linking
    env = Environment(ENV=os.environ,CPPPATH='.',CCFLAGS=ccflags, LINKFLAGS='-static', LIBS=['crypto', 'ssl', 'z', 'dl'])
    env.ParseConfig('apr-config --cflags --includes --libs --link-ld')
    env.ParseConfig('pkg-config --cflags libssl libcrypto')
    env.ParseConfig('pkg-config --cflags --libs lua5.2')
else:
    # dynamic linking
    env = Environment(ENV=os.environ,CPPPATH='.',CCFLAGS=ccflags, LIBS=['z'])
    env.ParseConfig('apr-config --cflags --includes --libs --link-ld')
    env.ParseConfig('pkg-config --cflags --libs libssl libcrypto')
    env.ParseConfig('pkg-config --cflags --libs lua5.2')

env.Program('jrs', map(lambda x: env.Object(x), Glob('*.c')))
