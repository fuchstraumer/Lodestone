"""Writes the table of the Vulkan support each SPIR-V capability and extension needs.

The cook stores the SPIR-V capability and extension names that each entry point needs. A Vulkan
renderer must turn each name into the device support that permits it. The `<spirvcapabilities>` and
`<spirvextensions>` sections of `vk.xml` hold that map. Each name lists one or more `<enable>`
entries, and the device needs any one of them:

    version     a Vulkan version enables the name alone (`VK_VERSION_1_1`)
    extension   a device extension enables it (`VK_KHR_16bit_storage`)
    feature     a feature member of a feature struct (`VkPhysicalDeviceVulkan12Features`, `shaderFloat16`)
    property    a property member holds a bit or a value (`subgroupSupportedOperations`, `VK_SUBGROUP_FEATURE_SHUFFLE_BIT`)

SPIR-V gives some capabilities a second name (`StorageUniform16` is `UniformAndStorageBuffer16BitAccess`), and
SPIRV-Headers' `CapabilityToString` can return either one. `vk.xml` uses the primary name. So the script also reads
`spirv.core.grammar.json` and writes each alias with its primary name, and the cook stores the primary name.

The output is a client header of strings, sorted by name. It names no Vulkan type, so a client needs
no Vulkan header to read it. The file is checked in, so a build needs neither Python nor `vk.xml`.
Run this script again after a change to the Vulkan headers, and review the diff.

Usage:

    python scripts/generate-vulkan-capability-table.py third_party/dawn/third_party/vulkan-headers/src/registry/vk.xml third_party/dawn/third_party/spirv-headers/src/include/spirv/unified1/spirv.core.grammar.json client/include/VulkanCapabilityTable.hpp
"""

import argparse
import json
import pathlib
import re
from xml.etree import ElementTree


def ReadHeaderVersion(tree):
    # the first definition is the Vulkan one, and a later one belongs to Vulkan SC
    for type_element in tree.iter('type'):
        name = type_element.find('name')
        if name is not None and name.text == 'VK_HEADER_VERSION' and 'vulkan' in type_element.get('api', 'vulkan').split(','):
            return int(''.join(type_element.itertext()).split()[-1])
    raise SystemExit('vk.xml holds no VK_HEADER_VERSION')


def ParseVersion(version_name):
    match = re.fullmatch(r'VK_VERSION_(\d+)_(\d+)', version_name)
    if match is None:
        raise SystemExit(f'not a Vulkan version name: {version_name}')
    return int(match.group(1)), int(match.group(2))


def ReadEnable(enable):
    """One `<enable>` element as (kind, name, member, value, requires)."""
    if enable.get('version') is not None:
        return ('version', enable.get('version'), '', '', '')
    if enable.get('extension') is not None:
        return ('extension', enable.get('extension'), '', '', '')
    if enable.get('struct') is not None:
        return ('feature', enable.get('struct'), enable.get('feature'), '', enable.get('requires', ''))
    if enable.get('property') is not None:
        return ('property', enable.get('property'), enable.get('member'), enable.get('value'), enable.get('requires', ''))
    raise SystemExit(f'unknown <enable> form: {enable.attrib}')


def ReadEntries(tree):
    """Each SPIR-V name with its enables. Capabilities and extensions share one list: an extension name
    starts with `SPV_`, so the two never collide."""
    entries = {}
    for section, element_name in (('spirvcapabilities', 'spirvcapability'), ('spirvextensions', 'spirvextension')):
        for element in tree.find(section).findall(element_name):
            name = element.get('name')
            if name in entries:
                raise SystemExit(f'{name} is listed twice')
            entries[name] = [ReadEnable(enable) for enable in element.findall('enable')]
    return dict(sorted(entries.items()))


def ReadAliases(grammar_path):
    """Each capability alias with its primary name, sorted by the alias."""
    grammar = json.loads(pathlib.Path(grammar_path).read_text(encoding='utf-8'))
    capability_kind = next(kind for kind in grammar['operand_kinds'] if kind['kind'] == 'Capability')
    aliases = {}
    for enumerant in capability_kind['enumerants']:
        for alias in enumerant.get('aliases', []):
            aliases[alias] = enumerant['enumerant']
    return dict(sorted(aliases.items()))


def VersionCode(major, minor):
    # the same packing as VK_MAKE_API_VERSION(0, major, minor, 0), so a renderer compares its own api version
    return (major << 22) | (minor << 12)


def WriteHeader(entries, aliases, header_version, output_path):
    lines = []
    lines.append('#pragma once')
    lines.append('#ifndef LODESTONE_VULKAN_CAPABILITY_TABLE_HPP')
    lines.append('#define LODESTONE_VULKAN_CAPABILITY_TABLE_HPP')
    lines.append('#include <algorithm>')
    lines.append('#include <array>')
    lines.append('#include <cstdint>')
    lines.append('#include <optional>')
    lines.append('#include <span>')
    lines.append('#include <string_view>')
    lines.append('')
    lines.append('/**')
    lines.append(' * Generated by scripts/generate-vulkan-capability-table.py from vk.xml, header version '
                 f'{header_version}. Do not edit.')
    lines.append(' *')
    lines.append(' * The Vulkan support that each SPIR-V capability and extension needs. A device that has any one')
    lines.append(' * enable of a name can run code that declares that name. The strings are the names of the Vulkan')
    lines.append(' * headers, so a renderer tests each one against its own feature and property structs.')
    lines.append(' */')
    lines.append('namespace lodestone::vulkan')
    lines.append('{')
    lines.append('')
    lines.append(f'inline constexpr uint32_t k_CapabilityTableHeaderVersion = {header_version}u;')
    lines.append('')
    lines.append('enum class CapabilityEnableKind : uint8_t')
    lines.append('{')
    lines.append('    /** @brief A Vulkan version enables the name alone. `Name` is the version, such as `VK_VERSION_1_1`. */')
    lines.append('    Version,')
    lines.append('    /** @brief A device extension enables the name. `Name` is the extension. */')
    lines.append('    Extension,')
    lines.append('    /** @brief A feature enables the name. `Name` is the feature struct, and `Member` is the feature. */')
    lines.append('    Feature,')
    lines.append('    /** @brief A property enables the name. `Name` is the property struct, `Member` is the member, and `Value`')
    lines.append('     * is the bit or the value it must hold. */')
    lines.append('    Property,')
    lines.append('};')
    lines.append('')
    lines.append('struct CapabilityEnable')
    lines.append('{')
    lines.append('    CapabilityEnableKind Kind;')
    lines.append('    std::string_view Name;')
    lines.append('    std::string_view Member;')
    lines.append('    std::string_view Value;')
    lines.append('    /** @brief The versions or extensions that make the struct available, separated by commas. Empty for a')
    lines.append('     * version or an extension. */')
    lines.append('    std::string_view Requires;')
    lines.append('};')
    lines.append('')
    lines.append('/** @brief One SPIR-V capability or extension name, and its run of enables. */')
    lines.append('struct CapabilityEntry')
    lines.append('{')
    lines.append('    std::string_view SpirvName;')
    lines.append('    /** @brief The lowest Vulkan version that enables the name alone, packed as `VK_MAKE_API_VERSION(0, major,')
    lines.append('     * minor, 0)`. Zero when no version does. */')
    lines.append('    uint32_t CoreVersion;')
    lines.append('    uint32_t FirstEnable;')
    lines.append('    uint32_t EnableCount;')
    lines.append('};')
    lines.append('')

    enables = []
    rows = []
    for name, entry_enables in entries.items():
        versions = [ParseVersion(enable[1]) for enable in entry_enables if enable[0] == 'version']
        core = VersionCode(*min(versions)) if versions else 0
        rows.append((name, core, len(enables), len(entry_enables)))
        enables.extend(entry_enables)

    kind_names = {'version': 'Version', 'extension': 'Extension', 'feature': 'Feature', 'property': 'Property'}
    lines.append(f'inline constexpr std::array<CapabilityEnable, {len(enables)}u> k_CapabilityEnables{{ {{')
    for kind, name, member, value, requires in enables:
        lines.append(f'    {{ CapabilityEnableKind::{kind_names[kind]}, "{name}", "{member}", "{value}", "{requires}" }},')
    lines.append('} };')
    lines.append('')
    lines.append('/** @brief Sorted by `SpirvName`. */')
    lines.append(f'inline constexpr std::array<CapabilityEntry, {len(rows)}u> k_CapabilityEntries{{ {{')
    for name, core, first, count in rows:
        lines.append(f'    {{ "{name}", {core}u, {first}u, {count}u }},')
    lines.append('} };')
    lines.append('')
    lines.append('[[nodiscard]] constexpr std::optional<CapabilityEntry> FindCapability(std::string_view spirv_name) noexcept')
    lines.append('{')
    lines.append('    const auto position = std::ranges::lower_bound(k_CapabilityEntries, spirv_name, {}, &CapabilityEntry::SpirvName);')
    lines.append('    if (position == k_CapabilityEntries.end() || position->SpirvName != spirv_name)')
    lines.append('    {')
    lines.append('        return std::nullopt;')
    lines.append('    }')
    lines.append('    return *position;')
    lines.append('}')
    lines.append('')
    lines.append('/** @brief One SPIR-V capability alias and its primary name, the name `vk.xml` uses. */')
    lines.append('struct CapabilityAlias')
    lines.append('{')
    lines.append('    std::string_view Alias;')
    lines.append('    std::string_view Name;')
    lines.append('};')
    lines.append('')
    lines.append('/** @brief Sorted by `Alias`. */')
    lines.append(f'inline constexpr std::array<CapabilityAlias, {len(aliases)}u> k_CapabilityAliases{{ {{')
    for alias, name in aliases.items():
        lines.append(f'    {{ "{alias}", "{name}" }},')
    lines.append('} };')
    lines.append('')
    lines.append('/** @brief The primary name of a SPIR-V capability: `name` itself, unless `name` is an alias. */')
    lines.append('[[nodiscard]] constexpr std::string_view PrimaryCapabilityName(std::string_view name) noexcept')
    lines.append('{')
    lines.append('    const auto position = std::ranges::lower_bound(k_CapabilityAliases, name, {}, &CapabilityAlias::Alias);')
    lines.append('    return position != k_CapabilityAliases.end() && position->Alias == name ? position->Name : name;')
    lines.append('}')
    lines.append('')
    lines.append('[[nodiscard]] constexpr std::span<const CapabilityEnable> EnablesOf(const CapabilityEntry& entry) noexcept')
    lines.append('{')
    lines.append('    return std::span<const CapabilityEnable>{ k_CapabilityEnables }.subspan(entry.FirstEnable, entry.EnableCount);')
    lines.append('}')
    lines.append('')
    lines.append('} // namespace lodestone::vulkan')
    lines.append('')
    lines.append('#endif // !LODESTONE_VULKAN_CAPABILITY_TABLE_HPP')
    lines.append('')

    pathlib.Path(output_path).write_text('\n'.join(lines), encoding='utf-8', newline='\r\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('vk_xml', help='the path of vk.xml')
    parser.add_argument('spirv_grammar', help='the path of spirv.core.grammar.json')
    parser.add_argument('output', help='the path of the header to write')
    arguments = parser.parse_args()

    tree = ElementTree.parse(arguments.vk_xml).getroot()
    entries = ReadEntries(tree)
    aliases = ReadAliases(arguments.spirv_grammar)
    WriteHeader(entries, aliases, ReadHeaderVersion(tree), arguments.output)
    print(f'{len(entries)} names, {sum(len(enables) for enables in entries.values())} enables, {len(aliases)} aliases')


if __name__ == '__main__':
    main()
