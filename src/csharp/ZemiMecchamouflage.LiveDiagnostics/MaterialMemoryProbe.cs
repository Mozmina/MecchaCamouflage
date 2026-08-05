using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace ZemiMecchamouflage.LiveDiagnostics;

/// <summary>
/// Read-only UE object inspection for the paint component already selected by the resident
/// bridge.  It deliberately uses only PROCESS_QUERY_INFORMATION | PROCESS_VM_READ and never
/// calls into the game or modifies a UObject.
/// </summary>
internal static class MaterialMemoryProbe
{
    private const uint ProcessQueryInformation = 0x0400;
    private const uint ProcessVmRead = 0x0010;
    private const uint NamePoolDeltaFromGuObjectArray = 0xE3B40;
    private const int ObjectClassOffset = 0x10;
    private const int ObjectNameOffset = 0x18;
    private const int StructSuperOffset = 0x40;
    private const int StructChildrenOffset = 0x48;
    private const int StructPropertiesSizeOffset = 0x58;
    private const int UFieldNextOffset = 0x28;
    private const int StructChildPropertiesOffset = 0x50;
    private const int FFieldNextOffset = 0x18;
    private const int FFieldNameOffset = 0x20;
    private const int FPropertyElementSizeOffset = 0x34;
    private const int FPropertyOffsetOffset = 0x44;

    private static readonly byte?[] GuObjectSignature =
    {
        0x48, 0x8D, 0x05, null, null, null, null, 0x48, 0x89, 0x01, 0x45, 0x8B, 0xD1
    };

    private static readonly IReadOnlyList<byte?[]> NamePoolPatterns =
    [
        [0x48, 0x8D, 0x0D, null, null, null, null, 0xE8, null, null, null, null, 0x4C, 0x8B, 0xC0],
        [0x48, 0x8D, 0x0D, null, null, null, null, 0xE8, null, null, null, null, 0x48, 0x8B],
        [0x48, 0x8D, 0x35, null, null, null, null],
        [0x48, 0x8D, 0x3D, null, null, null, null]
    ];

    private sealed record PropertyDescriptor(string Name, int Offset, int ElementSize, ulong Field);

    public static object Capture(int processId, ulong component)
    {
        using var process = Process.GetProcessById(processId);
        if (process.HasExited)
            throw new InvalidOperationException("The game process exited before material inspection.");
        var module = process.MainModule ?? throw new InvalidOperationException("The game main module is unavailable.");
        using var memory = new ProcessMemory(processId);
        var image = memory.ReadExact((ulong)module.BaseAddress.ToInt64(), module.ModuleMemorySize);
        var signatureOffset = FindSignature(image, GuObjectSignature);
        if (signatureOffset < 0)
            throw new InvalidOperationException("The UE object-array signature was not found in the current game build.");
        var relative = BitConverter.ToInt32(image, signatureOffset + 3);
        var guObjectArray = checked((ulong)(module.BaseAddress.ToInt64() + signatureOffset + 7L + relative));
        var namePool = checked(guObjectArray - NamePoolDeltaFromGuObjectArray);
        var names = new FNamePoolReader(memory, namePool);
        if (!names.IsUsable)
        {
            foreach (var candidatePattern in NamePoolPatterns)
            {
                var candidateOffset = FindSignature(image, candidatePattern);
                if (candidateOffset < 0)
                    continue;
                var candidateRelative = BitConverter.ToInt32(image, candidateOffset + 3);
                var candidatePool = checked((ulong)(module.BaseAddress.ToInt64() + candidateOffset + 7L + candidateRelative));
                names = new FNamePoolReader(memory, candidatePool);
                if (names.IsUsable)
                    break;
            }
        }
        if (!names.IsUsable)
            throw new InvalidOperationException(
                "The UE name pool could not be validated in the current game build (guobject=0x" +
                guObjectArray.ToString("x") + ", primary_name_pool=0x" + namePool.ToString("x") + ").");

        var componentClass = memory.ReadUInt64(component + ObjectClassOffset);
        var dynamicMaterial = memory.ReadUInt64(component + 416);
        var customBrushMaterial = memory.ReadUInt64(component + 296);
        var brushDynamicMaterial = memory.ReadUInt64(component + 568);
        var targetMesh = memory.ReadUInt64(component + 576);
        var parameterNames = new Dictionary<string, string>
        {
            ["albedo"] = names.Resolve(memory.ReadUInt32(component + 320)),
            ["material_properties"] = names.Resolve(memory.ReadUInt32(component + 328)),
            ["metallic"] = names.Resolve(memory.ReadUInt32(component + 336)),
            ["roughness"] = names.Resolve(memory.ReadUInt32(component + 344)),
            ["height"] = names.Resolve(memory.ReadUInt32(component + 352)),
            ["emissive"] = names.Resolve(memory.ReadUInt32(component + 360))
        };
        var renderTargets = new Dictionary<string, object?>
        {
            ["albedo"] = DescribeObject(memory, names, memory.ReadUInt64(component + 368)),
            ["material_properties"] = DescribeObject(memory, names, memory.ReadUInt64(component + 376)),
            ["metallic"] = DescribeObject(memory, names, memory.ReadUInt64(component + 384)),
            ["roughness"] = DescribeObject(memory, names, memory.ReadUInt64(component + 392)),
            ["height"] = DescribeObject(memory, names, memory.ReadUInt64(component + 400)),
            ["emissive"] = DescribeObject(memory, names, memory.ReadUInt64(component + 408))
        };
        var materialProperties = EnumerateProperties(memory, names, dynamicMaterial)
            .Where(property => property.Name.Contains("parameter", StringComparison.OrdinalIgnoreCase) ||
                               property.Name.Contains("texture", StringComparison.OrdinalIgnoreCase) ||
                               property.Name.Contains("parent", StringComparison.OrdinalIgnoreCase) ||
                               property.Name.Contains("emissive", StringComparison.OrdinalIgnoreCase) ||
                               property.Name.Contains("material", StringComparison.OrdinalIgnoreCase))
            .Take(96)
            .ToArray();
        var parentProperty = EnumerateProperties(memory, names, dynamicMaterial)
            .FirstOrDefault(property => string.Equals(property.Name, "Parent", StringComparison.OrdinalIgnoreCase));
        var parent = parentProperty is null || parentProperty.Offset < 0
            ? 0UL
            : memory.ReadUInt64(dynamicMaterial + (ulong)parentProperty.Offset);
        var dynamicParameters = InspectDynamicParameters(memory, names, dynamicMaterial, materialProperties);

        return new
        {
            read_only = true,
            component = DescribeObject(memory, names, component),
            component_class = DescribeObject(memory, names, componentClass),
            parameter_names = parameterNames,
            render_targets = renderTargets,
            native_paint_initialization_state = ReadNativePaintInitializationState(memory, component),
            dynamic_material = DescribeObject(memory, names, dynamicMaterial),
            dynamic_material_parent = DescribeObject(memory, names, parent),
            custom_brush_material = DescribeObject(memory, names, customBrushMaterial),
            brush_dynamic_material = DescribeObject(memory, names, brushDynamicMaterial),
            target_mesh_component = DescribeObject(memory, names, targetMesh),
            declared_paint_functions = EnumerateRelevantFunctions(memory, names, componentClass),
            dynamic_material_properties = materialProperties,
            dynamic_material_parameters = dynamicParameters
        };
    }

    private static object? DescribeObject(ProcessMemory memory, FNamePoolReader names, ulong address)
    {
        if (address == 0)
            return null;
        try
        {
            var classAddress = memory.ReadUInt64(address + ObjectClassOffset);
            return new
            {
                address = "0x" + address.ToString("x"),
                name = names.Resolve(memory.ReadUInt32(address + ObjectNameOffset)),
                class_name = classAddress == 0 ? "" : names.Resolve(memory.ReadUInt32(classAddress + ObjectNameOffset))
            };
        }
        catch (InvalidOperationException)
        {
            return new { address = "0x" + address.ToString("x"), unreadable = true };
        }
    }

    private static IReadOnlyList<PropertyDescriptor> EnumerateProperties(
        ProcessMemory memory,
        FNamePoolReader names,
        ulong objectAddress)
    {
        if (objectAddress == 0)
            return [];
        var properties = new List<PropertyDescriptor>();
        var type = memory.ReadUInt64(objectAddress + ObjectClassOffset);
        var seen = new HashSet<ulong>();
        for (var depth = 0; type != 0 && depth < 32; depth++)
        {
            for (var field = memory.ReadUInt64(type + StructChildPropertiesOffset);
                 field != 0 && seen.Add(field) && properties.Count < 512;
                 field = memory.ReadUInt64(field + FFieldNextOffset))
            {
                var name = names.Resolve(memory.ReadUInt32(field + FFieldNameOffset));
                if (string.IsNullOrWhiteSpace(name))
                    continue;
                properties.Add(new PropertyDescriptor(
                    name,
                    memory.ReadInt32(field + FPropertyOffsetOffset),
                    memory.ReadInt32(field + FPropertyElementSizeOffset),
                    field));
            }
            type = memory.ReadUInt64(type + StructSuperOffset);
        }
        return properties;
    }

    private static object[] EnumerateRelevantFunctions(ProcessMemory memory, FNamePoolReader names, ulong componentClass)
    {
        if (componentClass == 0)
            return [];
        var results = new List<object>();
        var seen = new HashSet<ulong>();
        for (var child = memory.ReadUInt64(componentClass + StructChildrenOffset);
             child != 0 && seen.Add(child) && results.Count < 128;
             child = memory.ReadUInt64(child + UFieldNextOffset))
        {
            var name = names.Resolve(memory.ReadUInt32(child + ObjectNameOffset));
            if (!IsPaintMaterialRelevantFunction(name))
                continue;
            results.Add(new
            {
                name,
                parameter_bytes = memory.ReadInt32(child + StructPropertiesSizeOffset)
            });
        }
        return results.ToArray();
    }

    private static object ReadNativePaintInitializationState(ProcessMemory memory, ulong component)
    {
        // These are private native fields, verified against the current InitializePaint machine code.
        // We name them by offset rather than guessing semantic labels from a reverse-engineered layout.
        var integerFields = new Dictionary<string, object>();
        foreach (var offset in new[] { 0xb8, 0xc0, 0xc4, 0xc8, 0xcc, 0xd0 })
        {
            var raw = memory.ReadInt32(component + (ulong)offset);
            integerFields["offset_0x" + offset.ToString("x")] = new
            {
                signed = raw,
                unsigned_value = unchecked((uint)raw),
                as_float = BitConverter.ToSingle(BitConverter.GetBytes(raw), 0)
            };
        }
        var floatFields = new Dictionary<string, float>();
        foreach (var offset in new[] { 0xdc, 0xe0, 0xe4, 0xe8, 0xec, 0xf0, 0xf4, 0xf8, 0xfc, 0x100, 0x104 })
            floatFields["offset_0x" + offset.ToString("x")] = BitConverter.ToSingle(memory.ReadExact(component + (ulong)offset, sizeof(float)));
        return new
        {
            read_only = true,
            initialization_flag_offset_0x220 = memory.ReadExact(component + 0x220, 1)[0] != 0,
            material_properties_mode_flag_offset_0xbc = memory.ReadExact(component + 0xbc, 1)[0] != 0,
            integer_fields = integerFields,
            float_fields = floatFields
        };
    }

    private static bool IsPaintMaterialRelevantFunction(string name)
    {
        if (string.IsNullOrWhiteSpace(name))
            return false;
        var normalized = name.ToLowerInvariant();
        return normalized.Contains("paint", StringComparison.Ordinal) ||
               normalized.Contains("material", StringComparison.Ordinal) ||
               normalized.Contains("texture", StringComparison.Ordinal) ||
               normalized.Contains("emissive", StringComparison.Ordinal) ||
               normalized.Contains("rendertarget", StringComparison.Ordinal) ||
               normalized.Contains("initialize", StringComparison.Ordinal) ||
               normalized.Contains("setup", StringComparison.Ordinal) ||
               normalized.Contains("create", StringComparison.Ordinal);
    }

    private static object InspectDynamicParameters(
        ProcessMemory memory,
        FNamePoolReader names,
        ulong material,
        IReadOnlyList<PropertyDescriptor> materialProperties) => new
    {
        texture = InspectParameterArray(memory, names, material, materialProperties, "TextureParameterValues", "texture"),
        scalar = InspectParameterArray(memory, names, material, materialProperties, "ScalarParameterValues", "scalar"),
        vector = InspectParameterArray(memory, names, material, materialProperties, "VectorParameterValues", "vector")
    };

    private static object InspectParameterArray(
        ProcessMemory memory,
        FNamePoolReader names,
        ulong material,
        IReadOnlyList<PropertyDescriptor> materialProperties,
        string propertyName,
        string valueKind)
    {
        var array = materialProperties.FirstOrDefault(property =>
            string.Equals(property.Name, propertyName, StringComparison.OrdinalIgnoreCase));
        if (array is null || array.Offset < 0)
            return new { available = false, reason = "array_property_unavailable", values = Array.Empty<object>() };
        var data = memory.ReadUInt64(material + (ulong)array.Offset);
        var count = memory.ReadInt32(material + (ulong)array.Offset + 8);
        var capacity = memory.ReadInt32(material + (ulong)array.Offset + 12);
        if (count < 0 || count > 256 || capacity < count || capacity > 256 || (count > 0 && data == 0))
        {
            return new { available = false, reason = "array_layout_invalid", count, capacity, values = Array.Empty<object>() };
        }
        var elementStruct = TryResolveArrayElementStruct(memory, names, array.Field);
        if (elementStruct == 0)
            return new { available = false, reason = "array_element_schema_unavailable", count, capacity, values = Array.Empty<object>() };
        var fields = EnumerateStructFields(memory, names, elementStruct);
        var info = fields.FirstOrDefault(field => string.Equals(field.Name, "ParameterInfo", StringComparison.OrdinalIgnoreCase));
        var value = fields.FirstOrDefault(field => string.Equals(field.Name, "ParameterValue", StringComparison.OrdinalIgnoreCase));
        if (info is null || value is null || info.Offset < 0 || value.Offset < 0)
        {
            return new
            {
                available = false,
                reason = "parameter_value_schema_unavailable",
                count,
                capacity,
                element_fields = fields.Select(field => new { field.Name, field.Offset, field.ElementSize }).ToArray(),
                values = Array.Empty<object>()
            };
        }
        var infoStruct = memory.ReadUInt64(info.Field + 0x70);
        var infoName = infoStruct == 0
            ? null
            : EnumerateStructFields(memory, names, infoStruct).FirstOrDefault(field =>
                string.Equals(field.Name, "Name", StringComparison.OrdinalIgnoreCase));
        if (infoName is null || infoName.Offset < 0)
            return new { available = false, reason = "parameter_name_schema_unavailable", count, capacity, values = Array.Empty<object>() };

        // The Inner FProperty is authoritative for TArray stride; using the parent array's
        // 16-byte TArray element size would read arbitrary memory after the first entry.
        var stride = TryReadArrayElementSize(memory, array.Field);
        if (stride is < 1 or > 512)
            return new { available = false, reason = "parameter_stride_unavailable", count, capacity, values = Array.Empty<object>() };
        var values = new List<object>();
        for (var index = 0; index < count; index++)
        {
            var entry = data + (ulong)(index * stride);
            var parameterName = names.Resolve(memory.ReadUInt32(entry + (ulong)info.Offset + (ulong)infoName.Offset));
            object? parameterValue = valueKind switch
            {
                "texture" => DescribeObject(memory, names, memory.ReadUInt64(entry + (ulong)value.Offset)),
                "scalar" => BitConverter.ToSingle(memory.ReadExact(entry + (ulong)value.Offset, sizeof(float))),
                "vector" => ReadVector(memory, entry + (ulong)value.Offset),
                _ => "unsupported"
            };
            values.Add(new { index, parameter = parameterName, value = parameterValue });
        }
        return new
        {
            available = true,
            count,
            capacity,
            stride,
            element_fields = fields.Select(field => new { field.Name, field.Offset, field.ElementSize }).ToArray(),
            values
        };
    }

    private static ulong TryResolveArrayElementStruct(ProcessMemory memory, FNamePoolReader names, ulong arrayField)
    {
        foreach (var innerOffset in new[] { 0x68, 0x70, 0x78, 0x80, 0x88 })
        {
            var inner = memory.ReadUInt64(arrayField + (ulong)innerOffset);
            if (inner == 0)
                continue;
            var structure = memory.ReadUInt64(inner + 0x70);
            if (structure == 0 || EnumerateStructFields(memory, names, structure).Count == 0)
                continue;
            return structure;
        }
        return 0;
    }

    private static int TryReadArrayElementSize(ProcessMemory memory, ulong arrayField)
    {
        foreach (var innerOffset in new[] { 0x68, 0x70, 0x78, 0x80, 0x88 })
        {
            var inner = memory.ReadUInt64(arrayField + (ulong)innerOffset);
            if (inner == 0)
                continue;
            var size = memory.ReadInt32(inner + FPropertyElementSizeOffset);
            if (size is > 0 and <= 512)
                return size;
        }
        return 0;
    }

    private static IReadOnlyList<PropertyDescriptor> EnumerateStructFields(
        ProcessMemory memory,
        FNamePoolReader names,
        ulong structure)
    {
        if (structure == 0)
            return [];
        var fields = new List<PropertyDescriptor>();
        var seen = new HashSet<ulong>();
        for (var field = memory.ReadUInt64(structure + StructChildPropertiesOffset);
             field != 0 && seen.Add(field) && fields.Count < 96;
             field = memory.ReadUInt64(field + FFieldNextOffset))
        {
            var name = names.Resolve(memory.ReadUInt32(field + FFieldNameOffset));
            if (string.IsNullOrWhiteSpace(name))
                continue;
            fields.Add(new PropertyDescriptor(
                name,
                memory.ReadInt32(field + FPropertyOffsetOffset),
                memory.ReadInt32(field + FPropertyElementSizeOffset),
                field));
        }
        return fields;
    }

    private static object ReadVector(ProcessMemory memory, ulong address)
    {
        var values = memory.ReadExact(address, sizeof(float) * 4);
        return new
        {
            r = BitConverter.ToSingle(values, 0),
            g = BitConverter.ToSingle(values, 4),
            b = BitConverter.ToSingle(values, 8),
            a = BitConverter.ToSingle(values, 12)
        };
    }

    private static int FindSignature(IReadOnlyList<byte> bytes, IReadOnlyList<byte?> pattern)
    {
        for (var offset = 0; offset <= bytes.Count - pattern.Count; offset++)
        {
            var match = true;
            for (var index = 0; index < pattern.Count; index++)
            {
                if (pattern[index] is byte expected && bytes[offset + index] != expected)
                {
                    match = false;
                    break;
                }
            }
            if (match)
                return offset;
        }
        return -1;
    }

    private sealed class FNamePoolReader
    {
        private readonly ProcessMemory memory;
        private readonly ulong pool;
        private readonly int[] offsets = [0x8, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70];
        private int tableOffset = 0x10;
        private int style = 1;

        public FNamePoolReader(ProcessMemory memory, ulong pool)
        {
            this.memory = memory;
            this.pool = pool;
            foreach (var candidateOffset in offsets)
            {
                foreach (var candidateStyle in new[] { 2, 1, 0 })
                {
                    if (Entry(0, candidateOffset, candidateStyle) != "None")
                        continue;
                    tableOffset = candidateOffset;
                    style = candidateStyle;
                    IsUsable = true;
                    return;
                }
            }
        }

        public bool IsUsable { get; }

        public string Resolve(uint id)
        {
            var value = Entry(id, tableOffset, style);
            if (!string.IsNullOrWhiteSpace(value))
                return value;
            foreach (var candidateOffset in offsets)
            {
                foreach (var candidateStyle in new[] { 2, 1, 0 })
                {
                    value = Entry(id, candidateOffset, candidateStyle);
                    if (string.IsNullOrWhiteSpace(value))
                        continue;
                    tableOffset = candidateOffset;
                    style = candidateStyle;
                    return value;
                }
            }
            return "";
        }

        private string Entry(uint id, int table, int entryStyle)
        {
            try
            {
                var block = memory.ReadUInt64(pool + (ulong)table + (ulong)(id >> 16) * 8UL);
                if (block == 0)
                    return "";
                var entry = block + ((ulong)(id & 0xffff) << 1);
                var header = memory.ReadUInt16(entry);
                var wide = false;
                int length;
                if (entryStyle == 0)
                {
                    wide = (header & 1) != 0;
                    length = header >> 1;
                }
                else if (entryStyle == 2)
                {
                    wide = (header & 1) != 0;
                    length = (header >> 6) & 0x3ff;
                }
                else
                {
                    length = header & 0x3ff;
                    wide = ((header >> 10) & 1) != 0;
                }
                if (length is <= 0 or > 512)
                    return "";
                var bytes = memory.ReadExact(entry + 2, wide ? length * 2 : length);
                return wide
                    ? string.Concat(Enumerable.Range(0, length).Select(index => (char)BitConverter.ToUInt16(bytes, index * 2)))
                    : System.Text.Encoding.ASCII.GetString(bytes);
            }
            catch (InvalidOperationException)
            {
                return "";
            }
        }
    }

    private sealed class ProcessMemory : IDisposable
    {
        private readonly IntPtr handle;

        public ProcessMemory(int processId)
        {
            handle = OpenProcess(ProcessQueryInformation | ProcessVmRead, false, processId);
            if (handle == IntPtr.Zero)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not open the game process for read-only inspection.");
        }

        public byte[] ReadExact(ulong address, int count)
        {
            var bytes = new byte[count];
            if (!ReadProcessMemory(handle, (IntPtr)address, bytes, bytes.Length, out var read) || read.ToInt64() != bytes.Length)
                throw new InvalidOperationException("A required game-memory range was unreadable.");
            return bytes;
        }

        public ushort ReadUInt16(ulong address) => BitConverter.ToUInt16(ReadExact(address, sizeof(ushort)));
        public uint ReadUInt32(ulong address) => BitConverter.ToUInt32(ReadExact(address, sizeof(uint)));
        public ulong ReadUInt64(ulong address) => BitConverter.ToUInt64(ReadExact(address, sizeof(ulong)));
        public int ReadInt32(ulong address) => BitConverter.ToInt32(ReadExact(address, sizeof(int)));

        public void Dispose()
        {
            if (handle != IntPtr.Zero)
                _ = CloseHandle(handle);
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr OpenProcess(uint desiredAccess, bool inheritHandle, int processId);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool ReadProcessMemory(
            IntPtr process,
            IntPtr baseAddress,
            [Out] byte[] buffer,
            int size,
            out IntPtr numberOfBytesRead);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr handle);
    }
}
