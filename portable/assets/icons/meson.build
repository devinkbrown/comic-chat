# Canonical modern runtime assets. The parent portable/meson.build should enter
# this file only after `scripts/build-modern-icons.py verify` succeeds.

modern_app_id = 'io.github.devinkbrown.ComicChatReinked'
modern_icon_runtime_install_dir = get_option('datadir') / 'comic-chat-reinked' / 'icons'
installed_modern_icon_dir = get_option('prefix') / modern_icon_runtime_install_dir

modern_icon_sizes = [16, 20, 24, 32, 48, 64, 128, 256]
foreach size : modern_icon_sizes
    size_name = '@0@'.format(size)
    source_icon = 'generated/png/chat/' + size_name + '.png'
    install_data(
        source_icon,
        install_dir: modern_icon_runtime_install_dir / 'chat',
        rename: size_name + '.png',
    )
    install_data(
        source_icon,
        install_dir: get_option('datadir') / 'icons/hicolor' / (
            size_name + 'x' + size_name
        ) / 'apps',
        rename: modern_app_id + '.png',
    )
endforeach

modern_strip_sizes = [16, 20, 24, 32, 40, 48]
modern_strip_families = [
    'toolbar',
    'tabbar',
    'balloons',
    'member',
    'texttool',
    'usertool',
    'connect',
    'oldnew',
    'inactive',
    'active',
    'stopped',
]
foreach family : modern_strip_families
    foreach size : modern_strip_sizes
        size_name = '@0@'.format(size)
        install_data(
            'generated/png/strips/' + family + '/' + size_name + '.png',
            install_dir: modern_icon_runtime_install_dir / 'strips' / family,
            rename: size_name + '.png',
        )
    endforeach
endforeach

modern_expression_sizes = [
    '20x26',
    '25x33',
    '30x39',
    '40x52',
    '50x65',
    '60x78',
    '80x104',
]
modern_expression_names = [
    'happy',
    'coy',
    'bored',
    'scared',
    'sad',
    'angry',
    'shout',
    'laugh',
]
foreach expression : modern_expression_names
    foreach size_name : modern_expression_sizes
        install_data(
            'generated/png/expressions/' + expression + '/' + size_name + '.png',
            install_dir: modern_icon_runtime_install_dir / 'expressions' / expression,
            rename: size_name + '.png',
        )
    endforeach
endforeach

install_data(
    '../../packaging/linux/' + modern_app_id + '.desktop',
    install_dir: get_option('datadir') / 'applications',
)
install_data(
    '../../packaging/linux/' + modern_app_id + '.metainfo.xml',
    install_dir: get_option('datadir') / 'metainfo',
)
