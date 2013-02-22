import os

ccflags = '-O3 -g'

env = Environment(ENV=os.environ,CPPPATH='.',CCFLAGS=ccflags, LINKFLAGS='-static', LIBS=['crypto', 'ssl', 'z', 'dl'])

env.ParseConfig('apr-config --cflags --includes --libs --link-ld')
env.ParseConfig('pkg-config --cflags libssl libcrypto')

env.Program('jrs', map(lambda x: env.Object(x), Glob('*.c')))
