import os
import os.path

ccflags = '-O3 -g'
static = True
aprconf = 'apr-config'

if os.path.exists('/etc/fedora-release'):
    static = False
    aprconf = 'apr-1-config'


if static:
    # static linking
    env = Environment(ENV=os.environ,CPPPATH='.',CCFLAGS=ccflags, LINKFLAGS='-static', LIBS=['crypto', 'ssl', 'z', 'dl'])
    env.ParseConfig(aprconf + ' --cflags --includes --libs --link-ld')
    env.ParseConfig('pkg-config --cflags libssl libcrypto')
else:
    # dynamic linking
    env = Environment(ENV=os.environ,CPPPATH='.',CCFLAGS=ccflags, LIBS=['z'])
    env.ParseConfig(aprconf + ' --cflags --includes --libs --link-ld')
    env.ParseConfig('pkg-config --cflags --libs libssl libcrypto')

env.Program('jrs', map(lambda x: env.Object(x), Glob('*.c')))
