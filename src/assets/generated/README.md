# Generated AVB package — Tiki HD v1

`tiki-reimagined-hd-v1.avb` is a native Comic Chat simple-avatar package, not
a PNG preview. It contains an icon and six `AK_NBODIES2` whole-body records:
neutral, laugh, surprised, angry, sad, and action. The records preserve the
historical `CAvatarSimple::Save` wire layout and use embedded 24-bit DIBs.

## Source and reproducibility

| Input | SHA-256 |
| --- | --- |
| `../avatar-pose-sheets-v1/tiki/pose-00.png` | `cc01d2a30e77dc8572c6418eadbd5c21e9af587a839575cc1ff63355e7c695d4` |
| `../avatar-pose-sheets-v1/tiki/pose-01.png` | `ae20e93b218863f966edf2ba11fa61a8073cb90301b6856417f8b7f1c81454a0` |
| `../avatar-pose-sheets-v1/tiki/pose-02.png` | `713650bc66b981226807b99068e4556b35a009380414d680e8e2130d4a31668b` |
| `../avatar-pose-sheets-v1/tiki/pose-03.png` | `90029c380fbe23edde65c4ca849c47c475b7e76706d319f3319bb88dc85c5872` |
| `../avatar-pose-sheets-v1/tiki/pose-04.png` | `32197f7e2063d1a093dc397edc85182c23e082c7ece2944f2e62a4b4f94b1533` |
| `../avatar-pose-sheets-v1/tiki/pose-05.png` | `1bcd04f02699f72025d22e47b216a5178532eb5581ac2bfe40f9a241a8d0ada6` |

The artwork is an original, project-generated reimagining. It is not a
Microsoft asset and does not alter the provenance record for the legacy AVB
library. The packaging tool is [`tools/package_generated_avb.py`](../../../tools/package_generated_avb.py).
The current output SHA-256 is
`8edd70fe28a3a2ba27dc73c720f2d99e1f4eeb716d8cf88364d60606524d3ade`.

Rebuild it after installing the repository's pinned Pillow dependency:

```sh
python3 tools/package_generated_avb.py \
  --name "Tiki HD" \
  --copyright "ComicChat generated artwork, 2026" \
  --output src/assets/generated/tiki-reimagined-hd-v1.avb \
  assets/generated/avatar-pose-sheets-v1/tiki/pose-00.png \
  assets/generated/avatar-pose-sheets-v1/tiki/pose-01.png \
  assets/generated/avatar-pose-sheets-v1/tiki/pose-02.png \
  assets/generated/avatar-pose-sheets-v1/tiki/pose-03.png \
  assets/generated/avatar-pose-sheets-v1/tiki/pose-04.png \
  assets/generated/avatar-pose-sheets-v1/tiki/pose-05.png
```
