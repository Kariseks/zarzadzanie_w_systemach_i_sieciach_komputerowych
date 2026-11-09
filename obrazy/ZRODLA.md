# Obrazy testowe – źródła

Programy wczytują każdy z tych plików przez `-i obrazy/<plik>` i od razu konwertują go do skali szarości.
Do pomiarów zależności od rozmiaru (E13) mniejsze wersje powstają z `nasa_blue_marble_…` skryptem
`analiza/przygotuj_obrazy.py` w katalogu `obrazy/skalowane/` (nie commitowany, odtwarzalny).

| Plik | Wymiary | Źródło | Licencja / podpis | SHA-256 |
|------|---------|--------|-------------------|---------|
| `lena_gray.gif` | 512×512 | klasyczny obraz testowy (USC-SIPI) | obraz testowy; od 2024 r. IEEE nie przyjmuje z nim publikacji – w sprawozdaniu tylko obok drugiego obrazu | `1de60ad9…d984aa` |
| `lena_color.gif` | 512×512 | jw. | jw. | `1bc96ecf…69fe5e` |
| `l_hires.jpg` | 1084×2318 | nieznane | **do sprawdzenia przed użyciem w sprawozdaniu** (proporcje inne niż standardowy kadr 512×512) | `820eae76…ed46` |
| `kodim08.png` | 768×512 | Kodak Lossless True Color Image Suite, https://r0k.us/graphics/kodak/, pobrano 08.11.2026 | udostępniony przez Kodak do swobodnego użytku | `ba23983c…d6e7` |
| `kodim23.png` | 768×512 | jw. | jw. | `e3111a2f…5baf` |
| `nasa_blue_marble_200407_21600x10800.jpg` | 21600×10800 (233 Mpix) | NASA Blue Marble Next Generation, lipiec 2004, z topografią i batymetrią; https://science.nasa.gov/earth/earth-observatory/blue-marble-next-generation/base-topography-bathymetry, pobrano 08.11.2026 | domena publiczna; podpis: „NASA Earth Observatory” | `d225f1f3…a7b0` |

Plik NASA (27 MB) nie jest commitowany (`obrazy/.gitignore`). Pobranie od nowa:

```bash
curl -L -o obrazy/nasa_blue_marble_200407_21600x10800.jpg \
  https://assets.science.nasa.gov/content/dam/science/esd/eo/images/bmng/bmng-topography-bathymetry/july/world.topo.bathy.200407.3x21600x10800.jpg
sha256sum obrazy/nasa_blue_marble_200407_21600x10800.jpg
# d225f1f35a6448a4d1d8f6de6e48f3433e470085b70a35800e64f384f269a7b0
```
