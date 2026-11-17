// SPDX-License-Identifier: GPL-3.0-or-later
//
// Periodic-table data, adapted from NeoCalculator (NumOS) by El-EnderJ:
// https://github.com/El-EnderJ/NeoCalculator, src/apps/ChemDatabase.h,
// GPL-3.0-or-later. The element values, category enum and 18x10 layout are the
// upstream ones; the NAV_LUT/UI are not carried over.
#pragma once

#include <stdint.h>

namespace periodic
{
enum class Category : uint8_t
{
    NonMetal,
    NobleGas,
    AlkaliMetal,
    AlkalineEarth,
    Metalloid,
    Halogen,
    TransitionMetal,
    PostTransitionMetal,
    Lanthanide,
    Actinide,
    Unknown
};

struct Element
{
    uint8_t z;
    const char *symbol;
    const char *name;
    float mass;
    Category category;
    float electronegativity;
    const char *config;
};

constexpr Category NM = Category::NonMetal;
constexpr Category NG = Category::NobleGas;
constexpr Category AM = Category::AlkaliMetal;
constexpr Category AE = Category::AlkalineEarth;
constexpr Category ML = Category::Metalloid;
constexpr Category HL = Category::Halogen;
constexpr Category TM = Category::TransitionMetal;
constexpr Category PT = Category::PostTransitionMetal;
constexpr Category LN = Category::Lanthanide;
constexpr Category AC = Category::Actinide;
constexpr Category UK = Category::Unknown;

constexpr Element kElements[118] = {
    {1, "H", "Hydrogen", 1.008f, NM, 2.2f, "1s1"},
    {2, "He", "Helium", 4.003f, NG, 0.0f, "1s2"},
    {3, "Li", "Lithium", 6.941f, AM, 0.98f, "[He] 2s1"},
    {4, "Be", "Beryllium", 9.012f, AE, 1.57f, "[He] 2s2"},
    {5, "B", "Boron", 10.81f, ML, 2.04f, "[He] 2s2 2p1"},
    {6, "C", "Carbon", 12.011f, NM, 2.55f, "[He] 2s2 2p2"},
    {7, "N", "Nitrogen", 14.007f, NM, 3.04f, "[He] 2s2 2p3"},
    {8, "O", "Oxygen", 15.999f, NM, 3.44f, "[He] 2s2 2p4"},
    {9, "F", "Fluorine", 18.998f, HL, 3.98f, "[He] 2s2 2p5"},
    {10, "Ne", "Neon", 20.18f, NG, 0.0f, "[He] 2s2 2p6"},
    {11, "Na", "Sodium", 22.99f, AM, 0.93f, "[Ne] 3s1"},
    {12, "Mg", "Magnesium", 24.305f, AE, 1.31f, "[Ne] 3s2"},
    {13, "Al", "Aluminum", 26.982f, PT, 1.61f, "[Ne] 3s2 3p1"},
    {14, "Si", "Silicon", 28.085f, ML, 1.9f, "[Ne] 3s2 3p2"},
    {15, "P", "Phosphorus", 30.974f, NM, 2.19f, "[Ne] 3s2 3p3"},
    {16, "S", "Sulfur", 32.06f, NM, 2.58f, "[Ne] 3s2 3p4"},
    {17, "Cl", "Chlorine", 35.45f, HL, 3.16f, "[Ne] 3s2 3p5"},
    {18, "Ar", "Argon", 39.948f, NG, 0.0f, "[Ne] 3s2 3p6"},
    {19, "K", "Potassium", 39.098f, AM, 0.82f, "[Ar] 4s1"},
    {20, "Ca", "Calcium", 40.078f, AE, 1.0f, "[Ar] 4s2"},
    {21, "Sc", "Scandium", 44.956f, TM, 1.36f, "[Ar] 3d1 4s2"},
    {22, "Ti", "Titanium", 47.867f, TM, 1.54f, "[Ar] 3d2 4s2"},
    {23, "V", "Vanadium", 50.942f, TM, 1.63f, "[Ar] 3d3 4s2"},
    {24, "Cr", "Chromium", 51.996f, TM, 1.66f, "[Ar] 3d5 4s1"},
    {25, "Mn", "Manganese", 54.938f, TM, 1.55f, "[Ar] 3d5 4s2"},
    {26, "Fe", "Iron", 55.845f, TM, 1.83f, "[Ar] 3d6 4s2"},
    {27, "Co", "Cobalt", 58.933f, TM, 1.88f, "[Ar] 3d7 4s2"},
    {28, "Ni", "Nickel", 58.693f, TM, 1.91f, "[Ar] 3d8 4s2"},
    {29, "Cu", "Copper", 63.546f, TM, 1.9f, "[Ar] 3d10 4s1"},
    {30, "Zn", "Zinc", 65.38f, TM, 1.65f, "[Ar] 3d10 4s2"},
    {31, "Ga", "Gallium", 69.723f, PT, 1.81f, "[Ar] 3d10 4s2 4p1"},
    {32, "Ge", "Germanium", 72.63f, ML, 2.01f, "[Ar] 3d10 4s2 4p2"},
    {33, "As", "Arsenic", 74.922f, ML, 2.18f, "[Ar] 3d10 4s2 4p3"},
    {34, "Se", "Selenium", 78.971f, NM, 2.55f, "[Ar] 3d10 4s2 4p4"},
    {35, "Br", "Bromine", 79.904f, HL, 2.96f, "[Ar] 3d10 4s2 4p5"},
    {36, "Kr", "Krypton", 83.798f, NG, 3.0f, "[Ar] 3d10 4s2 4p6"},
    {37, "Rb", "Rubidium", 85.468f, AM, 0.82f, "[Kr] 5s1"},
    {38, "Sr", "Strontium", 87.62f, AE, 0.95f, "[Kr] 5s2"},
    {39, "Y", "Yttrium", 88.906f, TM, 1.22f, "[Kr] 4d1 5s2"},
    {40, "Zr", "Zirconium", 91.224f, TM, 1.33f, "[Kr] 4d2 5s2"},
    {41, "Nb", "Niobium", 92.906f, TM, 1.6f, "[Kr] 4d4 5s1"},
    {42, "Mo", "Molybdenum", 95.95f, TM, 2.16f, "[Kr] 4d5 5s1"},
    {43, "Tc", "Technetium", 98.0f, TM, 1.9f, "[Kr] 4d5 5s2"},
    {44, "Ru", "Ruthenium", 101.07f, TM, 2.2f, "[Kr] 4d7 5s1"},
    {45, "Rh", "Rhodium", 102.91f, TM, 2.28f, "[Kr] 4d8 5s1"},
    {46, "Pd", "Palladium", 106.42f, TM, 2.2f, "[Kr] 4d10"},
    {47, "Ag", "Silver", 107.87f, TM, 1.93f, "[Kr] 4d10 5s1"},
    {48, "Cd", "Cadmium", 112.41f, TM, 1.69f, "[Kr] 4d10 5s2"},
    {49, "In", "Indium", 114.82f, PT, 1.78f, "[Kr] 4d10 5s2 5p1"},
    {50, "Sn", "Tin", 118.71f, PT, 1.96f, "[Kr] 4d10 5s2 5p2"},
    {51, "Sb", "Antimony", 121.76f, ML, 2.05f, "[Kr] 4d10 5s2 5p3"},
    {52, "Te", "Tellurium", 127.6f, ML, 2.1f, "[Kr] 4d10 5s2 5p4"},
    {53, "I", "Iodine", 126.9f, HL, 2.66f, "[Kr] 4d10 5s2 5p5"},
    {54, "Xe", "Xenon", 131.29f, NG, 2.6f, "[Kr] 4d10 5s2 5p6"},
    {55, "Cs", "Cesium", 132.91f, AM, 0.79f, "[Xe] 6s1"},
    {56, "Ba", "Barium", 137.33f, AE, 0.89f, "[Xe] 6s2"},
    {57, "La", "Lanthanum", 138.91f, LN, 1.1f, "[Xe] 5d1 6s2"},
    {58, "Ce", "Cerium", 140.12f, LN, 1.12f, "[Xe] 4f1 5d1 6s2"},
    {59, "Pr", "Praseodymium", 140.91f, LN, 1.13f, "[Xe] 4f3 6s2"},
    {60, "Nd", "Neodymium", 144.24f, LN, 1.14f, "[Xe] 4f4 6s2"},
    {61, "Pm", "Promethium", 145.0f, LN, 1.13f, "[Xe] 4f5 6s2"},
    {62, "Sm", "Samarium", 150.36f, LN, 1.17f, "[Xe] 4f6 6s2"},
    {63, "Eu", "Europium", 151.96f, LN, 1.2f, "[Xe] 4f7 6s2"},
    {64, "Gd", "Gadolinium", 157.25f, LN, 1.2f, "[Xe] 4f7 5d1 6s2"},
    {65, "Tb", "Terbium", 158.93f, LN, 1.1f, "[Xe] 4f9 6s2"},
    {66, "Dy", "Dysprosium", 162.5f, LN, 1.22f, "[Xe] 4f10 6s2"},
    {67, "Ho", "Holmium", 164.93f, LN, 1.23f, "[Xe] 4f11 6s2"},
    {68, "Er", "Erbium", 167.26f, LN, 1.24f, "[Xe] 4f12 6s2"},
    {69, "Tm", "Thulium", 168.93f, LN, 1.25f, "[Xe] 4f13 6s2"},
    {70, "Yb", "Ytterbium", 173.05f, LN, 1.1f, "[Xe] 4f14 6s2"},
    {71, "Lu", "Lutetium", 174.97f, LN, 1.27f, "[Xe] 4f14 5d1 6s2"},
    {72, "Hf", "Hafnium", 178.49f, TM, 1.3f, "[Xe] 4f14 5d2 6s2"},
    {73, "Ta", "Tantalum", 180.95f, TM, 1.5f, "[Xe] 4f14 5d3 6s2"},
    {74, "W", "Tungsten", 183.84f, TM, 2.36f, "[Xe] 4f14 5d4 6s2"},
    {75, "Re", "Rhenium", 186.21f, TM, 1.9f, "[Xe] 4f14 5d5 6s2"},
    {76, "Os", "Osmium", 190.23f, TM, 2.2f, "[Xe] 4f14 5d6 6s2"},
    {77, "Ir", "Iridium", 192.22f, TM, 2.2f, "[Xe] 4f14 5d7 6s2"},
    {78, "Pt", "Platinum", 195.08f, TM, 2.28f, "[Xe] 4f14 5d9 6s1"},
    {79, "Au", "Gold", 196.97f, TM, 2.54f, "[Xe] 4f14 5d10 6s1"},
    {80, "Hg", "Mercury", 200.59f, TM, 2.0f, "[Xe] 4f14 5d10 6s2"},
    {81, "Tl", "Thallium", 204.38f, PT, 1.62f, "[Xe] 4f14 5d10 6s2 6p1"},
    {82, "Pb", "Lead", 207.2f, PT, 1.87f, "[Xe] 4f14 5d10 6s2 6p2"},
    {83, "Bi", "Bismuth", 208.98f, PT, 2.02f, "[Xe] 4f14 5d10 6s2 6p3"},
    {84, "Po", "Polonium", 209.0f, PT, 2.0f, "[Xe] 4f14 5d10 6s2 6p4"},
    {85, "At", "Astatine", 210.0f, HL, 2.2f, "[Xe] 4f14 5d10 6s2 6p5"},
    {86, "Rn", "Radon", 222.0f, NG, 2.2f, "[Xe] 4f14 5d10 6s2 6p6"},
    {87, "Fr", "Francium", 223.0f, AM, 0.7f, "[Rn] 7s1"},
    {88, "Ra", "Radium", 226.0f, AE, 0.9f, "[Rn] 7s2"},
    {89, "Ac", "Actinium", 227.0f, AC, 1.1f, "[Rn] 6d1 7s2"},
    {90, "Th", "Thorium", 232.04f, AC, 1.3f, "[Rn] 6d2 7s2"},
    {91, "Pa", "Protactinium", 231.04f, AC, 1.5f, "[Rn] 5f2 6d1 7s2"},
    {92, "U", "Uranium", 238.03f, AC, 1.38f, "[Rn] 5f3 6d1 7s2"},
    {93, "Np", "Neptunium", 237.0f, AC, 1.36f, "[Rn] 5f4 6d1 7s2"},
    {94, "Pu", "Plutonium", 244.0f, AC, 1.28f, "[Rn] 5f6 7s2"},
    {95, "Am", "Americium", 243.0f, AC, 1.3f, "[Rn] 5f7 7s2"},
    {96, "Cm", "Curium", 247.0f, AC, 1.3f, "[Rn] 5f7 6d1 7s2"},
    {97, "Bk", "Berkelium", 247.0f, AC, 1.3f, "[Rn] 5f9 7s2"},
    {98, "Cf", "Californium", 251.0f, AC, 1.3f, "[Rn] 5f10 7s2"},
    {99, "Es", "Einsteinium", 252.0f, AC, 1.3f, "[Rn] 5f11 7s2"},
    {100, "Fm", "Fermium", 257.0f, AC, 1.3f, "[Rn] 5f12 7s2"},
    {101, "Md", "Mendelevium", 258.0f, AC, 1.3f, "[Rn] 5f13 7s2"},
    {102, "No", "Nobelium", 259.0f, AC, 1.3f, "[Rn] 5f14 7s2"},
    {103, "Lr", "Lawrencium", 266.0f, AC, 1.3f, "[Rn] 5f14 7s2 7p1"},
    {104, "Rf", "Rutherfordium", 267.0f, TM, 0.0f, "[Rn] 5f14 6d2 7s2"},
    {105, "Db", "Dubnium", 268.0f, TM, 0.0f, "[Rn] 5f14 6d3 7s2"},
    {106, "Sg", "Seaborgium", 269.0f, TM, 0.0f, "[Rn] 5f14 6d4 7s2"},
    {107, "Bh", "Bohrium", 270.0f, TM, 0.0f, "[Rn] 5f14 6d5 7s2"},
    {108, "Hs", "Hassium", 277.0f, TM, 0.0f, "[Rn] 5f14 6d6 7s2"},
    {109, "Mt", "Meitnerium", 278.0f, UK, 0.0f, "[Rn] 5f14 6d7 7s2"},
    {110, "Ds", "Darmstadtium", 281.0f, UK, 0.0f, "[Rn] 5f14 6d9 7s1"},
    {111, "Rg", "Roentgenium", 282.0f, UK, 0.0f, "[Rn] 5f14 6d10 7s1"},
    {112, "Cn", "Copernicium", 285.0f, TM, 0.0f, "[Rn] 5f14 6d10 7s2"},
    {113, "Nh", "Nihonium", 286.0f, UK, 0.0f, "[Rn] 5f14 6d10 7s2 7p1"},
    {114, "Fl", "Flerovium", 289.0f, UK, 0.0f, "[Rn] 5f14 6d10 7s2 7p2"},
    {115, "Mc", "Moscovium", 290.0f, UK, 0.0f, "[Rn] 5f14 6d10 7s2 7p3"},
    {116, "Lv", "Livermorium", 293.0f, UK, 0.0f, "[Rn] 5f14 6d10 7s2 7p4"},
    {117, "Ts", "Tennessine", 294.0f, UK, 0.0f, "[Rn] 5f14 6d10 7s2 7p5"},
    {118, "Og", "Oganesson", 294.0f, UK, 0.0f, "[Rn] 5f14 6d10 7s2 7p6"},
};

// Row 7 is intentionally empty: the f-block sits in the last two rows.
constexpr int8_t kLayout[10][18] = {
    {1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 2},
    {3, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 5, 6, 7, 8, 9, 10},
    {11, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 13, 14, 15, 16, 17, 18},
    {19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36},
    {37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54},
    {55, 56, -1, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86},
    {87, 88, -1, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118},
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {-1, -1, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, -1},
    {-1, -1, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, -1},
};
}  // namespace periodic
