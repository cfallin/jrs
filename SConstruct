import os

ccflags = '-g'

env = Environment(ENV=os.environ,CPPPATH='.',CCFLAGS=ccflags)
env.ParseConfig('apr-config --cflags --includes --libs --link-ld')

env.Program('jrs-daemon', map(lambda x: env.Object(x), Glob('*.c')))
