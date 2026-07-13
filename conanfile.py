from conan import ConanFile
from conan.tools.cmake import CMakeDeps, CMakeToolchain, cmake_layout


class SpotiampConan(ConanFile):
    name = "spotiamp"
    version = "0.0.1"
    package_type = "application"

    settings = "os", "arch", "compiler", "build_type"

    requires = (
        "sdl/2.32.10",
        "libcurl/8.21.0",
    )

    default_options = {
        "sdl/*:shared": False,
        "libcurl/*:shared": False,
    }

    def configure(self):
        if self.settings.os == "Windows":
            self.options["libcurl"].with_ssl = "schannel"
        elif self.settings.os == "Macos":
            self.options["libcurl"].with_ssl = "openssl"
        else:
            self.options["libcurl"].with_ssl = "openssl"

    def layout(self):
        cmake_layout(self)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()

        toolchain = CMakeToolchain(self)
        toolchain.generate()
