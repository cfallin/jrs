import os

ccflags = '-O3 -g'

env = Environment(ENV=os.environ,CPPPATH='.',CCFLAGS=ccflags)

env.ParseConfig('apr-config --cflags --includes --libs --link-ld')
env.ParseConfig('pkg-config --libs --cflags libssl libcrypto')

env.Program('jrs', map(lambda x: env.Object(x), Glob('*.c')))
