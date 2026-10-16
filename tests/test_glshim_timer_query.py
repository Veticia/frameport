"""GL shim: stand-ins for GPU timer queries a driver lacks (AXRB's emulator: gfxstream GLES has no
GL_EXT_disjoint_timer_query; Path of the Warrior's GLAD loader called a NULL glGenQueriesEXT)."""
import os
import subprocess

from test_blazerush import FIXTURES, ROOT, _cc, _khronos_headers

HOST_TEST = r'''
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
typedef void *(*GPA)(const char *);
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d\n", __LINE__); return 1; } } while (0)
int main(int argc, char **argv) {
    (void)argc;
    void *shim = dlopen(argv[1], RTLD_NOW);
    CHECK(shim);
    GPA gpa = (GPA)dlsym(shim, "eglGetProcAddress");
    void (*gen)(GLsizei, GLuint *) = (void (*)(GLsizei, GLuint *))gpa("glGenQueriesEXT");
    void (*objuiv)(GLuint, GLenum, GLuint *) = (void (*)(GLuint, GLenum, GLuint *))gpa("glGetQueryObjectuivEXT");
    void (*objui64v)(GLuint, GLenum, GLuint64 *) =
        (void (*)(GLuint, GLenum, GLuint64 *))gpa("glGetQueryObjectui64vEXT");
    void (*getiv)(GLenum, GLint *) = (void (*)(GLenum, GLint *))gpa("glGetIntegerv");
    CHECK(gen && objuiv && objui64v && gpa("glQueryCounterEXT") && gpa("glGetInteger64vEXT"));
    CHECK(!gpa("glNoSuchFunctionEXT"));  // only the timer-query names get stand-ins
    GLuint ids[2] = {0, 0};
    gen(2, ids);
    CHECK(ids[0] && ids[1] && ids[0] != ids[1]);
    GLuint avail = 0; GLuint64 ns = 99;
    objuiv(ids[0], 0x8867 /* GL_QUERY_RESULT_AVAILABLE */, &avail);
    objui64v(ids[0], 0x8866 /* GL_QUERY_RESULT */, &ns);
    CHECK(avail == 1 && ns == 0);
    GLint disjoint = 7;
    getiv(0x8FBB /* GL_GPU_DISJOINT_EXT */, &disjoint);
    CHECK(disjoint == 0);
    // listed, so GLAD loaders look the functions up at all
    const GLubyte *(*gs)(GLenum) = (const GLubyte *(*)(GLenum))gpa("glGetString");
    const GLubyte *(*gsi)(GLenum, GLuint) = (const GLubyte *(*)(GLenum, GLuint))gpa("glGetStringi");
    CHECK(strstr((const char *)gs(GL_EXTENSIONS), "GL_EXT_disjoint_timer_query"));
    GLint n = 0;
    getiv(GL_NUM_EXTENSIONS, &n);
    CHECK(n > 0 && !strcmp((const char *)gsi(GL_EXTENSIONS, (GLuint)(n - 1)), "GL_EXT_disjoint_timer_query"));
    CHECK(gsi(GL_EXTENSIONS, (GLuint)n) == NULL);
    printf("OK\n");
    return 0;
}
'''


def test_timer_query_standins(tmp_path):
    cc = _cc()
    inc = _khronos_headers(tmp_path / "include")
    src, out = FIXTURES / "src", tmp_path / "lib"
    out.mkdir()
    warn = ["-Wall", "-Wextra", "-Werror"]
    subprocess.run([cc, "-std=gnu11", *warn, "-shared", "-fPIC", "-fvisibility=hidden", "-I", str(inc),
                    str(src / "glshim_fake_gles.c"), "-Wl,-soname,libglshim_fake.so", "-ldl",
                    "-o", str(out / "libglshim_fake.so")], check=True)
    subprocess.run([cc, "-std=gnu11", *warn, "-Wno-unused-function", "-Wno-unused-variable",
                    "-Wno-unused-but-set-variable", "-Wno-incompatible-pointer-types", "-shared", "-fPIC",
                    "-fvisibility=hidden", "-DEGL_NO_X11", '-DGLSHIM_GLES_LIB="libglshim_fake.so"',
                    '-DGLSHIM_EGL_LIB="libglshim_fake.so"', "-I", str(src / "host_include"), "-I", str(inc),
                    str(ROOT / "native/glshim/glshim.c"), "-Wl,--no-as-needed", "-L", str(out), "-lglshim_fake",
                    "-Wl,--as-needed", "-Wl,-rpath,$ORIGIN", "-ldl", "-lpthread",
                    "-o", str(out / "libglshim_host.so")], check=True)
    (tmp_path / "t.c").write_text(HOST_TEST)
    subprocess.run([cc, "-std=gnu11", *warn, "-DEGL_NO_X11", "-I", str(src / "host_include"), "-I", str(inc),
                    str(tmp_path / "t.c"), "-ldl", "-o", str(out / "t")], check=True)
    env = {k: v for k, v in os.environ.items() if k != "FRAMEBRIDGE_CONFIG"}
    run = subprocess.run([str(out / "t"), str(out / "libglshim_host.so")], capture_output=True, text=True, env=env)
    assert run.returncode == 0 and "OK" in run.stdout, run.stdout + run.stderr
    assert "no GPU timer queries in this driver: glGenQueriesEXT" in run.stderr
