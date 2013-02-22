import os

ccflags = '-O3 -g'

if True:
    # static linking
    env = Environment(ENV=os.environ,CPPPATH='.',CCFLAGS=ccflags, LINKFLAGS='-static', LIBS=['crypto', 'ssl', 'z', 'dl'])
    env.ParseConfig('apr-config --cflags --includes --libs --link-ld')
    env.ParseConfig('pkg-config --cflags libssl libcrypto')
else:
    # dynamic linking
    env = Environment(ENV=os.environ,CPPPATH='.',CCFLAGS=ccflags, LIBS=['z'])
    env.ParseConfig('apr-config --cflags --includes --libs --link-ld')
    env.ParseConfig('pkg-config --cflags --libs libssl libcrypto')

env.Program('jrs', map(lambda x: env.Object(x), Glob('*.c')))
