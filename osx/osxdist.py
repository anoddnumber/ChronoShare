#!/usr/bin/env python
# -*- coding: utf-8 -*-
#
#
# Loosely based on original bash-version by Sebastian Schlingmann (based, again, on a OSX application bundler
# by Thomas Keller).
#

import sys, os, string, re, shutil, plistlib, tempfile, exceptions, datetime, tarfile
from subprocess import Popen, PIPE
from optparse import OptionParser

options = None

def gitrev():
  return os.popen('git describe').read()[:-1]

def codesign(path):
  '''Call the codesign executable.'''

  if hasattr(path, 'isalpha'):
    path = (path,)
  for p in path:
    #p = Popen(('codesign', '--keychain', options.codesign_keychain, '--signature-size', '6400', '-vvvv', '-s', options.codesign, p))
    p = Popen(('codesign', '-vvvv', '-s', options.codesign, p))
    retval = p.wait()
    if retval != 0:
      return retval
  return 0

class AppBundle(object):

  def is_system_lib(self, lib):
    '''
      Is the library a system library, meaning that we should not include it in our bundle?
    '''
    if lib.startswith('/System/Library/'):
      return True
    if lib.startswith('/usr/lib/'):
      return True

    return False

  def is_dylib(self, lib):
    '''
      Is the library a dylib?
    '''
    return lib.endswith('.dylib')

  def get_framework_base(self, fw):
    '''
      Extracts the base .framework bundle path from a library in an abitrary place in a framework.
    '''
    paths = fw.split('/')
    for i, str in enumerate(paths):
      if str.endswith('.framework'):
        return '/'.join(paths[:i+1])
    return None

  def is_framework(self, lib):
    '''
      Is the library a framework?
    '''
    return bool(self.get_framework_base(lib))

  def get_binary_libs(self, path):
    '''
      Get a list of libraries that we depend on.
    '''
    m = re.compile('^\t(.*)\ \(.*$')
    libs = Popen(['otool', '-L', path], stdout=PIPE).communicate()[0]
    libs = string.split(libs, '\n')
    ret = []
    bn = os.path.basename(path)
    for line in libs:
      g = m.match(line)
      if g is not None:
        lib = g.groups()[0]
        if lib != bn:
          ret.append(lib)
    return ret

  def handle_libs(self):
    '''
      Copy non-system libraries that we depend on into our bundle, and fix linker
      paths so they are relative to our bundle.
    '''
    print ' * Taking care of libraries'

    # Does our fwpath exist?
    fwpath = os.path.join(os.path.abspath(self.bundle), 'Contents', 'Frameworks')
    if not os.path.exists(fwpath):
      os.mkdir(fwpath)

    self.handle_binary_libs()

    #actd = os.path.join(os.path.abspath(self.bundle), 'Contents', 'MacOS', 'actd')
    #if os.path.exists(actd):
    #  self.handle_binary_libs(actd)

  def handle_binary_libs(self, macho=None):
    '''
      Fix up dylib depends for a specific binary.
    '''
    print "macho is ", macho
    # Does our fwpath exist already? If not, create it.
    if not self.framework_path:
      self.framework_path = self.bundle + '/Contents/Frameworks'
      if not os.path.exists(self.framework_path):
        os.mkdir(self.framework_path)
      else:
        # shutil.rmtree(self.framework_path)
        # os.mkdir(self.framework_path)
        # do not remove /Contents/Framework; we have Sparkle.framework there
        pass

    # If we weren't explicitly told which binary to operate on, pick the
    # bundle's default executable from its property list.
    if macho is None:
      macho = os.path.abspath(self.binary)
    else:
      macho = os.path.abspath(macho)

    libs = self.get_binary_libs(macho)

    for lib in libs:

      # Skip system libraries
      if self.is_system_lib(lib):
        continue

      # Frameworks are 'special'.
      if self.is_framework(lib):
        fw_path = self.get_framework_base(lib)
        basename = os.path.basename(fw_path)
        name = basename.split('.framework')[0]
        rel = basename + '/' + name

        if fw_path.startswith('@loader_path') or fw_path.startswith('@executable_path'):
          continue

        abs = self.framework_path + '/' + rel

        if not basename in self.handled_libs:
          dst = self.framework_path + '/' + basename
          shutil.copytree(fw_path, dst, symlinks=True)
          if name.startswith('Qt'):
            try:
              os.remove(dst + '/Headers')
              os.remove(dst + '/' + name + '.prl')
              os.remove(dst + '/' + name + '_debug')
              os.remove(dst + '/' + name + '_debug.prl')
              shutil.rmtree(dst + '/Versions/4/Headers')
              os.remove(dst + '/Versions/4/' + name + '_debug')
            except OSError:
              pass
            os.chmod(abs, 0755)
            os.system('install_name_tool -id @executable_path/../Frameworks/%s %s' % (rel, abs))
            self.handled_libs[basename] = True
            self.handle_binary_libs(abs)
        os.chmod(macho, 0755)
        os.system('install_name_tool -change %s @executable_path/../Frameworks/%s %s' % (lib, rel, macho))

      # Regular dylibs
      else:
        basename = os.path.basename(lib)
        rel = basename

        if not basename in self.handled_libs:
          print lib
          print self.framework_path + '/' + basename
          try:
            shutil.copy(lib, self.framework_path  + '/' + basename)
          except IOError:
            print "IOError!" + self.framework_path + '/' + basename + "does not exist\n"
            continue
            
          abs = self.framework_path + '/' + rel
          os.chmod(abs, 0755)
          os.system('install_name_tool -id @executable_path/../Frameworks/%s %s' % (rel, abs))
          self.handled_libs[basename] = True
          self.handle_binary_libs(abs)
        os.chmod(macho, 0755)
        os.system('install_name_tool -change %s @executable_path/../Frameworks/%s %s' % (lib, rel, macho))

  def copy_resources(self, rsrcs):
    '''
      Copy needed resources into our bundle.
    '''
    print ' * Copying needed resources'
    rsrcpath = os.path.join(self.bundle, 'Contents', 'Resources')
    if not os.path.exists(rsrcpath):
      os.mkdir(rsrcpath)

    # Copy resources already in the bundle
    for rsrc in rsrcs:
      b = os.path.basename(rsrc)
      if os.path.isdir(rsrc):
                          shutil.copytree(rsrc, os.path.join(rsrcpath, b), symlinks=True)
      elif os.path.isfile(rsrc):
        shutil.copy(rsrc, os.path.join(rsrcpath, b))

    return

  def copy_qt_plugins(self):
    '''
      Copy over any needed Qt plugins.
    '''

    print ' * Copying Qt and preparing plugins'

    src = os.popen('qmake -query QT_INSTALL_PLUGINS').read().strip()
    dst = os.path.join(self.bundle, 'Contents', 'QtPlugins')
    shutil.copytree(src, dst, symlinks=False)

    top = dst
    files = {}

    def cb(arg, dirname, fnames):
      if dirname == top:
        return
      files[os.path.basename(dirname)] = fnames

    os.path.walk(top, cb, None)

    exclude = ( 'phonon_backend', 'designer', 'script' )

    for dir, files in files.items():
      absdir = dst + '/' + dir
      if dir in exclude:
        shutil.rmtree(absdir)
        continue
      for file in files:
        abs = absdir + '/' + file
        if file.endswith('_debug.dylib'):
          os.remove(abs)
        else:
          os.system('install_name_tool -id %s %s' % (file, abs))
          self.handle_binary_libs(abs)

  def update_plist(self):
    '''
      Modify our bundle's Info.plist to make it ready for release.
    '''
    if self.version is not None:
      print ' * Changing version in Info.plist'
      p = self.infoplist
      p['CFBundleVersion'] = self.version
      p['CFBundleExecutable'] = "ChronoShare"
      plistlib.writePlist(p, self.infopath)


  def set_min_macosx_version(self, version):
    '''
      Set the minimum version of Mac OS X version that this App will run on.
    '''
    print ' * Setting minimum Mac OS X version to: %s' % (version)
    self.infoplist['LSMinimumSystemVersion'] = version

  def done(self):
    plistlib.writePlist(self.infoplist, self.infopath)
    print ' * Done!'
    print ''

  def __init__(self, bundle, version=None):
    self.framework_path = ''
    self.handled_libs = {}
    self.bundle = bundle
    self.version = version
    self.infopath = os.path.join(os.path.abspath(bundle), 'Contents', 'Info.plist')
    self.infoplist = plistlib.readPlist(self.infopath)
    self.binary = os.path.join(os.path.abspath(bundle), 'Contents', 'MacOS', self.infoplist['CFBundleExecutable'])
    print ' * Preparing AppBundle'

class FolderObject(object):
  class Exception(exceptions.Exception):
    pass

  def __init__(self):
    self.tmp = tempfile.mkdtemp()

  def copy(self, src, dst='/'):
    '''
      Copy a file or directory into foler
    '''
    asrc = os.path.abspath(src)

    if dst[0] != '/':
      raise self.Exception

    # Determine destination
    if dst[-1] == '/':
      adst = os.path.abspath(self.tmp + '/' + dst + os.path.basename(src))
    else:
      adst = os.path.abspath(self.tmp + '/' + dst)

    if os.path.isdir(asrc):
      print ' * Copying directory: %s' % os.path.basename(asrc)
      shutil.copytree(asrc, adst, symlinks=True)
    elif os.path.isfile(asrc):
      print ' * Copying file: %s' % os.path.basename(asrc)
      shutil.copy(asrc, adst)

  def symlink(self, src, dst):
    '''
      Create a symlink inside the folder
    '''
    asrc = os.path.abspath(src)
    adst = self.tmp + '/' + dst
    print " * Creating symlink %s" % os.path.basename(asrc)
    os.symlink(asrc, adst)

  def mkdir(self, name):
    '''
      Create a directory inside the folder.
    '''
    print ' * Creating directory %s' % os.path.basename(name)
    adst = self.tmp + '/' + name
    os.makedirs(adst)

class DiskImage(FolderObject):
  
  def __init__(self, filename, volname):
    FolderObject.__init__(self)
    print ' * Preparing to create diskimage'
    self.filename = filename
    self.volname = volname

  def create(self):
    '''
      Create the disk image
    '''
    print ' * Creating disk image. Please wait...'
    if os.path.exists(self.filename):
      shutil.rmtree(self.filename)
    p = Popen(['hdiutil', 'create',
              '-srcfolder', self.tmp,
              '-format', 'UDBZ',
              '-volname', self.volname,
              self.filename])
      
    retval = p.wait()
    print ' * Removing temporary directory.'
    shutil.rmtree(self.tmp)
    print ' * Done!'


if __name__ == '__main__':
  parser = OptionParser()
  parser.add_option('', '--release', dest='release', help='Build a release. This determines the version number of the release.')
  parser.add_option('', '--snapshot', dest='snapshot', help='Build a snapshot release. This determines the \'snapshot version\'.')
  parser.add_option('', '--git', dest='git', help='Build a snapshot release. Use the git revision number as the \'snapshot version\'.', action='store_true', default=False)
  parser.add_option('', '--codesign', dest='codesign', help='Identity to use for code signing. (If not set, no code signing will occur)')
  parser.add_option('', '--codesign-keychain', dest='codesign_keychain', help='The keychain to use when invoking the codesign utility.')
  parser.add_option('', '--build-dir', dest='build_dir', help='specify build directory', default='build')
  parser.add_option('', '--dmg', dest='dmg', help='create dmg image', action='store_true', default=False)

  options, args = parser.parse_args()

  # Release
  if options.release:
    ver = options.release
  # Snapshot
  elif options.snapshot or options.git:
    if not options.git:
      ver = options.snapshot
    else:
      ver = gitrev()  
      #ver = "0.0.1"
  else:
    print 'Neither snapshot or release selected. Bailing.'
    sys.exit(1)

  if options.build_dir:
    os.chdir(options.build_dir)

  # Do the finishing touches to our Application bundle before release
  a = AppBundle('ChronoShare.app', ver)
  a.copy_qt_plugins()
  a.handle_libs()
  a.copy_resources(['../chronoshare.icns', '../gui/images', '../gui/html', '../osx/qt.conf', '../osx/dsa_pub.pem'])
  a.update_plist()
  a.set_min_macosx_version('10.8.0')
  a.done()

  # Sign our binaries, etc.
  if options.codesign:
    print ' * Signing binaries with identity `%s\'' % options.codesign
    binaries = (
      'ChronoShare.app',
    )

    codesign(binaries)
    print ''

  # Create diskimage
  if options.dmg:
    title = "ChronoShare-%s" % ver
    fn = "%s.dmg" % title
    d = DiskImage(fn, title)
    d.symlink('/Applications', '/Applications')
    d.copy('ChronoShare.app', '/ChronoShare.app')
    d.create()

