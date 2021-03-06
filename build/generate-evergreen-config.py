#!/usr/bin/env python
#
# Copyright 2018-present MongoDB, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Generate C Driver's config.yml for Evergreen testing.

We find that generating configuration from Python data structures and a template
file is more legible than Evergreen's matrix syntax or a handwritten file.

Written for Python 2.6+, requires Jinja 2 for templating.
"""

import sys
from collections import namedtuple, OrderedDict as OD
from itertools import product
from os.path import dirname, join as joinpath, normpath
from textwrap import dedent

try:
    import yaml
    import yamlordereddictloader
    from jinja2 import Environment, FileSystemLoader
except ImportError:
    sys.stderr.write("try 'pip install -r build/requirements.txt'")
    raise

this_dir = dirname(__file__)
evergreen_dir = normpath(joinpath(this_dir, '../.evergreen'))


# We want legible YAML tasks:
#
#     - name: debug-compile
#       tags: [zlib, snappy, compression, openssl]
#       commands:
#       - command: shell.exec
#         params:
#           script: |-
#             set -o errexit
#             set -o xtrace
#             ...
#
# Write values compactly except multiline strings, which use "|" style. Write
# tag sets as lists.

class Dumper(yamlordereddictloader.Dumper):
    def __init__(self, *args, **kwargs):
        super(Dumper, self).__init__(*args, **kwargs)
        self.add_representer(set, type(self).represent_set)

    def represent_scalar(self, tag, value, style=None):
        if isinstance(value, (str, unicode)) and '\n' in value:
            style = '|'
        return super(Dumper, self).represent_scalar(tag, value, style)

    def represent_set(self, data):
        return super(Dumper, self).represent_list(sorted(data))


class Task(object):
    def __init__(self, *args, **kwargs):
        super(Task, self).__init__()
        self.tags = set()
        self.options = OD()
        self.depends_on = []

    name_prefix = 'test'

    @property
    def name(self):
        return 'UNSET'

    def add_tags(self, *args):
        self.tags.update(args)

    def has_tags(self, *args):
        return bool(self.tags.intersection(args))

    def display(self, axis_name):
        value = getattr(self, axis_name)
        # E.g., if self.auth is False, return 'noauth'.
        if value is False:
            return 'no' + axis_name

        if value is True:
            return axis_name

        return value

    def on_off(self, *args, **kwargs):
        assert not (args and kwargs)
        if args:
            axis_name, = args
            return 'on' if getattr(self, axis_name) else 'off'

        (axis_name, value), = kwargs
        return 'on' if getattr(self, axis_name) == value else 'off'

    def to_dict(self):
        task = OD([('name', self.name), ('tags', self.tags)])
        task.update(self.options)
        if self.depends_on:
            task['depends_on'] = OD([
                ('name', name) for name in self.depends_on
            ])

        task['commands'] = commands = []
        if self.depends_on:
            commands.append(OD([
                ('func', 'fetch build'),
                ('vars', {'BUILD_NAME': self.depends_on[0]}),
            ]))
        return task

    def to_yaml(self):
        return yaml.dump(self.to_dict(), Dumper=Dumper)


class CompileTask(Task):
    def __init__(self, compile_task_name, tags=None, config='debug',
                 compression='default', continue_on_err=False,
                 extra_commands=None, **kwargs):
        super(CompileTask, self).__init__()
        self._compile_task_name = compile_task_name
        if tags:
            self.add_tags(*tags)

        self.extra_commands = extra_commands or []

        # Environment variables for .evergreen/compile.sh.
        self.compile_sh_opt = kwargs
        if config == 'debug':
            self.compile_sh_opt['DEBUG'] = 'ON'
        else:
            assert config == 'release'
            self.compile_sh_opt['RELEASE'] = 'ON'

        if compression != 'default':
            self.compile_sh_opt['SNAPPY'] = (
                'ON' if compression in ('all', 'snappy') else 'OFF')
            self.compile_sh_opt['ZLIB'] = (
                'BUNDLED' if compression in ('all', 'zlib') else 'OFF')

        self.continue_on_err = continue_on_err

    @property
    def name(self):
        return self._compile_task_name

    def to_dict(self):
        task = super(CompileTask, self).to_dict()

        script = "set -o errexit\nset -o xtrace\n"
        for opt, value in sorted(self.compile_sh_opt.items()):
            script += 'export %s="%s"\n' % (opt, value)

        script += "CC='${CC}' MARCH='${MARCH}' sh .evergreen/compile.sh"
        task['commands'].append(OD([
            ('command', 'shell.exec'),
            ('type', 'test'),
            ('params', OD([('working_dir', 'mongoc'), ('script', script)])),
        ]))

        task['commands'].append(OD([('func', 'upload build')]))
        task['commands'].extend(self.extra_commands)
        return task


class SpecialTask(CompileTask):
    def __init__(self, *args, **kwargs):
        super(SpecialTask, self).__init__(*args, **kwargs)
        self.add_tags('special')


compile_tasks = [
    CompileTask('debug-compile-compression-zlib',
                tags=['zlib', 'compression'],
                compression='zlib'),
    CompileTask('debug-compile-compression-snappy',
                tags=['snappy', 'compression'],
                compression='snappy'),
    CompileTask('debug-compile-compression',
                tags=['zlib', 'snappy', 'compression'],
                compression='all'),
    CompileTask('debug-compile-no-align',
                tags=['debug-compile'],
                compression='zlib',
                EXTRA_CONFIGURE_FLAGS="-DENABLE_EXTRA_ALIGNMENT=OFF"),
    CompileTask('debug-compile-nosasl-nossl',
                tags=['debug-compile', 'nosasl', 'nossl']),
    CompileTask('debug-compile-lto', CFLAGS='-flto'),
    CompileTask('debug-compile-lto-thin', CFLAGS='-flto=thin'),
    SpecialTask('debug-compile-c11',
                tags=['debug-compile', 'c11', 'stdflags'],
                CFLAGS='-std=c11 -D_XOPEN_SOURCE=600'),
    SpecialTask('debug-compile-c99',
                tags=['debug-compile', 'c99', 'stdflags'],
                CFLAGS='-std=c99 -D_XOPEN_SOURCE=600'),
    SpecialTask('debug-compile-c89',
                tags=['debug-compile', 'c89', 'stdflags'],
                CFLAGS='-std=c89 -D_POSIX_C_SOURCE=200112L -pedantic'),
    SpecialTask('debug-compile-valgrind',
                tags=['debug-compile', 'valgrind'],
                SASL='OFF',
                SSL='OPENSSL',
                VALGRIND='ON',
                CFLAGS='-DBSON_MEMCHECK'),
    SpecialTask('debug-compile-coverage',
                tags=['debug-compile', 'coverage'],
                COVERAGE='ON',
                extra_commands=[OD([('func', 'upload coverage')])]),
    CompileTask('debug-compile-no-counters',
                tags=['debug-compile', 'no-counters'],
                ENABLE_SHM_COUNTERS='OFF'),
    SpecialTask('debug-compile-asan-clang',
                tags=['debug-compile', 'asan-clang'],
                compression='zlib',
                CC='clang-3.8',
                CFLAGS='-fsanitize=address -fno-omit-frame-pointer'
                       ' -DBSON_MEMCHECK',
                CHECK_LOG='ON',
                EXTRA_CONFIGURE_FLAGS='-DENABLE_EXTRA_ALIGNMENT=OFF',
                PATH='/usr/lib/llvm-3.8/bin:$PATH'),
    # include -pthread in CFLAGS on gcc to address the issue explained here:
    # https://groups.google.com/forum/#!topic/address-sanitizer/JxnwgrWOLuc
    SpecialTask('debug-compile-asan-gcc',
                compression='zlib',
                CFLAGS='-fsanitize=address -pthread',
                CHECK_LOG='ON',
                EXTRA_CONFIGURE_FLAGS="-DENABLE_EXTRA_ALIGNMENT=OFF"),
    SpecialTask('debug-compile-asan-clang-openssl',
                tags=['debug-compile', 'asan-clang'],
                compression='zlib',
                CC='clang-3.8',
                CFLAGS='-fsanitize=address -fno-omit-frame-pointer'
                       ' -DBSON_MEMCHECK',
                CHECK_LOG='ON',
                EXTRA_CONFIGURE_FLAGS="-DENABLE_EXTRA_ALIGNMENT=OFF",
                PATH='/usr/lib/llvm-3.8/bin:$PATH',
                SSL='OPENSSL'),
    SpecialTask('debug-compile-ubsan',
                compression='zlib',
                CC='clang-3.8',
                CFLAGS='-fsanitize=undefined -fno-omit-frame-pointer'
                       ' -DBSON_MEMCHECK',
                CHECK_LOG='ON',
                EXTRA_CONFIGURE_FLAGS="-DENABLE_EXTRA_ALIGNMENT=OFF",
                PATH='/usr/lib/llvm-3.8/bin:$PATH'),
    SpecialTask('debug-compile-scan-build',
                tags=['clang', 'debug-compile', 'scan-build'],
                continue_on_err=True,
                ANALYZE='ON',
                CC='clang',
                extra_commands=[
                    OD([('func', 'upload scan artifacts')]),
                    OD([('command', 'shell.exec'),
                        ('type', 'test'),
                        ('params', OD([
                            ('working_dir', 'mongoc'),
                            ('script', dedent('''\
             if find scan -name \*.html | grep -q html; then
               exit 123
             fi'''))]))])]),
]

integration_task_axes = OD([
    ('valgrind', ['valgrind', False]),
    ('asan', ['asan', False]),
    ('coverage', ['coverage', False]),
    ('version', ['latest', '4.0', '3.6', '3.4', '3.2', '3.0']),
    ('topology', ['server', 'replica_set', 'sharded_cluster']),
    ('auth', [True, False]),
    ('sasl', ['sasl', 'sspi', False]),
    ('ssl', ['openssl', 'darwinssl', 'winssl', False]),
])


class IntegrationTask(Task, namedtuple('Task', tuple(integration_task_axes))):
    @property
    def name(self):
        def name_part(axis_name):
            part = self.display(axis_name)
            if part == 'replica_set':
                return 'replica-set'
            elif part == 'sharded_cluster':
                return 'sharded'
            return part

        return self.name_prefix + '-' + '-'.join(
            name_part(axis_name) for axis_name in integration_task_axes
            if getattr(self, axis_name) or axis_name in ('auth', 'sasl', 'ssl'))

    def to_dict(self):
        task = super(IntegrationTask, self).to_dict()
        commands = task['commands']
        if self.coverage:
            commands.append(OD([
                ('func', 'debug-compile-coverage-notest-%s-%s' % (
                    self.display('sasl'), self.display('ssl')
                )),
            ]))
        commands.append(OD([
            ('func', 'bootstrap mongo-orchestration'),
            ('vars', OD([
                ('VERSION', self.version),
                ('TOPOLOGY', self.topology),
                ('AUTH', 'auth' if self.auth else 'noauth'),
                ('SSL', self.display('ssl')),
            ])),
        ]))
        commands.append(OD([
            ('func', 'run tests'),
            ('vars', OD([
                ('VALGRIND', self.on_off('valgrind')),
                ('ASAN', self.on_off('asan')),
                ('AUTH', self.display('auth')),
                ('SSL', self.display('ssl')),
            ])),
        ]))
        if self.coverage:
            commands.append(OD([
                ('func', 'update codecov.io'),
            ]))

        return task


auth_task_axes = OD([
    ('sasl', ['sasl', 'sspi', False]),
    ('ssl', ['openssl', 'darwinssl', 'winssl']),
])


class AuthTask(Task, namedtuple('Task', tuple(auth_task_axes))):
    name_prefix = 'authentication-tests'

    @property
    def name(self):
        rv = self.name_prefix + '-' + self.display('ssl')
        if self.sasl:
            return rv
        else:
            return rv + '-nosasl'

    def to_dict(self):
        task = super(AuthTask, self).to_dict()
        task['commands'].append(OD([
            ('func', 'run auth tests'),
        ]))
        return task


def matrix(cell_class, axes):
    return set(cell_class(*cell) for cell in product(*axes.values()))


class Prohibited(Exception):
    pass


def require(rule):
    if not rule:
        raise Prohibited()


def prohibit(rule):
    if rule:
        raise Prohibited()


def both_or_neither(rule0, rule1):
    if rule0:
        require(rule1)
    else:
        prohibit(rule1)


def allow_integration_test_task(task):
    if task.valgrind:
        prohibit(task.asan)
        prohibit(task.sasl)
        require(task.ssl in ('openssl', False))
        prohibit(task.coverage)
        # Valgrind only with auth+SSL or no auth + no SSL.
        if task.auth:
            require(task.ssl == 'openssl')
        else:
            prohibit(task.ssl)

    if task.auth:
        require(task.ssl)

    if task.sasl == 'sspi':
        # Only one task.
        require(task.topology == 'server')
        require(task.version == 'latest')
        require(task.ssl == 'winssl')
        require(task.auth)

    if not task.ssl:
        prohibit(task.sasl)

    if task.coverage:
        prohibit(task.sasl)

        if task.auth:
            require(task.ssl == 'openssl')
        else:
            prohibit(task.ssl)

    if task.asan:
        prohibit(task.sasl)
        prohibit(task.coverage)

        # Address sanitizer only with auth+SSL or no auth + no SSL.
        if task.auth:
            require(task.ssl == 'openssl')
        else:
            prohibit(task.ssl)


def make_integration_test_tasks():
    tasks_list = []
    for task in matrix(IntegrationTask, integration_task_axes):
        try:
            allow_integration_test_task(task)
        except Prohibited:
            continue

        if task.valgrind:
            task.tags.add('test-valgrind')
            task.options['exec_timeout_secs'] = 7200
        elif task.coverage:
            task.tags.add('test-coverage')
            task.options['exec_timeout_secs'] = 3600
        elif task.asan:
            task.tags.add('test-asan')
            task.options['exec_timeout_secs'] = 3600
        else:
            task.tags.add(task.topology)
            task.tags.add(task.version)
            task.tags.update([task.display('ssl'),
                              task.display('sasl'),
                              task.display('auth')])

        # E.g., test-latest-server-auth-sasl-ssl needs debug-compile-sasl-ssl.
        # Coverage tasks use a build function instead of depending on a task.
        if task.valgrind:
            task.depends_on.append('debug-compile-valgrind')
        elif task.asan and task.ssl:
            task.depends_on.append('debug-compile-asan-clang-%s' % (
                task.display('ssl'),))
        elif task.asan:
            assert not task.sasl
            task.depends_on.append('debug-compile-asan-clang')
        elif not task.coverage:
            task.depends_on.append('debug-compile-%s-%s' % (
                task.display('sasl'), task.display('ssl')))

        tasks_list.append(task)

    return tasks_list


def allow_auth_test_task(task):
    both_or_neither(task.ssl == 'winssl', task.sasl == 'sspi')
    if not task.sasl:
        require(task.ssl == 'openssl')


def make_auth_test_tasks():
    tasks_list = []
    for task in matrix(AuthTask, auth_task_axes):
        try:
            allow_auth_test_task(task)
        except Prohibited:
            continue

        task.tags.update(['authentication-tests',
                          task.display('ssl'),
                          task.display('sasl')])

        task.depends_on.append('debug-compile-%s-%s' % (
            task.display('sasl'), task.display('ssl')))

        tasks_list.append(task)

    return tasks_list


env = Environment(loader=FileSystemLoader(this_dir),
                  trim_blocks=True,
                  lstrip_blocks=True,
                  extensions=['jinja2.ext.loopcontrols'])

env.filters['tag_list'] = lambda value: (
        '[' + ', '.join('"%s"' % (tag,) for tag in value) + ']')

print('.evergreen/config.yml')
f = open(joinpath(evergreen_dir, 'config.yml'), 'w+')
t = env.get_template('config.yml.template')
f.write(t.render(globals()))
f.write('\n')
