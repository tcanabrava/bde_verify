//@DEPRECATED://
template <class T> void h();
template <class T> void i();
struct C {
    template <class T> static void j();
    template <class T> static void k();
        // !DEPRECATED!:
    template <class T> static void l();
};
template <class PP>
struct D {
    template <class T> static void j();
    template <class T> static void k();
        // !DEPRECATED!:
    template <class T> static void l();
};
struct CC {
    // !DEPRECATED!:
    template <class T> static void j();
    template <class T> static void k();
        // !DEPRECATED!:
    template <class T> static void l();
};
template <class PP>
struct DD {
    // !DEPRECATED!:
    template <class T> static void j();
    template <class T> static void k();
        // !DEPRECATED!:
    template <class T> static void l();
};

// ----------------------------------------------------------------------------
// Copyright (C) 2016 Bloomberg Finance L.P.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// ----------------------------- END-OF-FILE ----------------------------------