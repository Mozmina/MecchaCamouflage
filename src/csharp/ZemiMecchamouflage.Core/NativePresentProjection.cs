using System.Threading;

namespace ZemiMecchamouflage.Core;

/// <summary>
/// Pure geometry and publication rules shared by the native-Present ESP
/// contract tests. The injected renderer mirrors these formulas exactly; no
/// process memory or Unreal object is touched here.
/// </summary>
public readonly record struct NativePresentVector3(double X, double Y, double Z)
{
    public static NativePresentVector3 operator +(NativePresentVector3 left, NativePresentVector3 right) =>
        new(left.X + right.X, left.Y + right.Y, left.Z + right.Z);

    public static NativePresentVector3 operator -(NativePresentVector3 left, NativePresentVector3 right) =>
        new(left.X - right.X, left.Y - right.Y, left.Z - right.Z);

    public static NativePresentVector3 operator *(NativePresentVector3 value, double scale) =>
        new(value.X * scale, value.Y * scale, value.Z * scale);
}

public readonly record struct NativePresentRotator(double Pitch, double Yaw, double Roll);

public enum NativePresentFovAxis
{
    MaintainY,
    MaintainX,
    MajorAxis
}

public readonly record struct NativePresentCamera(
    NativePresentVector3 Location,
    NativePresentRotator Rotation,
    double Fov,
    double AspectRatio,
    NativePresentFovAxis FovAxis);

public readonly record struct NativePresentCapsule(
    NativePresentVector3 Location,
    NativePresentRotator Rotation,
    double Radius,
    double HalfHeight);

public readonly record struct NativePresentPoint(double X, double Y);

public readonly record struct NativePresentRectangle(double Left, double Top, double Right, double Bottom)
{
    public double Width => Right - Left;
    public double Height => Bottom - Top;
}

public static class NativePresentProjection
{
    private const double DegreesToRadians = Math.PI / 180.0;

    public static bool TryProject(
        NativePresentCamera camera,
        NativePresentVector3 world,
        int viewportWidth,
        int viewportHeight,
        out NativePresentPoint screen)
    {
        screen = default;
        if (!IsFinite(camera.Location) || !IsFinite(world) ||
            !double.IsFinite(camera.Fov) || camera.Fov is < 1 or > 179 ||
            !double.IsFinite(camera.AspectRatio) || camera.AspectRatio is < 0.1 or > 100 ||
            viewportWidth <= 0 || viewportHeight <= 0)
            return false;

        var (forward, right, up) = GetAxes(camera.Rotation);
        var delta = world - camera.Location;
        var depth = Dot(delta, forward);
        if (!double.IsFinite(depth) || depth < 1.0)
            return false;

        var tangent = Math.Tan(camera.Fov * DegreesToRadians / 2.0);
        if (!double.IsFinite(tangent) || Math.Abs(tangent) < 0.000001)
            return false;

        var viewportAspect = (double)viewportWidth / viewportHeight;
        var axis = camera.FovAxis == NativePresentFovAxis.MajorAxis
            ? viewportAspect >= camera.AspectRatio ? NativePresentFovAxis.MaintainY : NativePresentFovAxis.MaintainX
            : camera.FovAxis;
        var focalY = axis == NativePresentFovAxis.MaintainY
            ? viewportHeight / 2.0 / tangent
            : viewportWidth / 2.0 / tangent / camera.AspectRatio;
        var focalX = axis == NativePresentFovAxis.MaintainY
            ? focalY * camera.AspectRatio
            : viewportWidth / 2.0 / tangent;

        screen = new NativePresentPoint(
            viewportWidth / 2.0 + Dot(delta, right) * focalX / depth,
            viewportHeight / 2.0 - Dot(delta, up) * focalY / depth);
        return double.IsFinite(screen.X) && double.IsFinite(screen.Y);
    }

    /// <summary>
    /// Projects surface points from the collision capsule. This intentionally
    /// ignores animated hands, feet, accessories, and mesh scaling changes.
    /// </summary>
    public static bool TryProjectCapsuleBounds(
        NativePresentCamera camera,
        NativePresentCapsule capsule,
        int viewportWidth,
        int viewportHeight,
        out NativePresentRectangle bounds)
    {
        bounds = default;
        if (!IsFinite(capsule.Location) || !double.IsFinite(capsule.Radius) || !double.IsFinite(capsule.HalfHeight) ||
            capsule.Radius <= 0 || capsule.HalfHeight < capsule.Radius)
            return false;

        var (forward, right, up) = GetAxes(capsule.Rotation);
        var cylinderHalfHeight = capsule.HalfHeight - capsule.Radius;
        Span<NativePresentVector3> samples = stackalloc NativePresentVector3[18];
        var count = 0;
        samples[count++] = capsule.Location + up * capsule.HalfHeight;
        samples[count++] = capsule.Location - up * capsule.HalfHeight;
        for (var ring = -1; ring <= 1; ring += 2)
        {
            var ringCentre = capsule.Location + up * (cylinderHalfHeight * ring);
            for (var step = 0; step < 8; ++step)
            {
                var radians = step * Math.PI / 4.0;
                samples[count++] = ringCentre +
                                   right * (Math.Cos(radians) * capsule.Radius) +
                                   forward * (Math.Sin(radians) * capsule.Radius);
            }
        }

        var any = false;
        var left = double.PositiveInfinity;
        var top = double.PositiveInfinity;
        var rightEdge = double.NegativeInfinity;
        var bottom = double.NegativeInfinity;
        for (var index = 0; index < count; ++index)
        {
            if (!TryProject(camera, samples[index], viewportWidth, viewportHeight, out var point))
                continue;
            any = true;
            left = Math.Min(left, point.X);
            top = Math.Min(top, point.Y);
            rightEdge = Math.Max(rightEdge, point.X);
            bottom = Math.Max(bottom, point.Y);
        }
        if (!any || !double.IsFinite(left) || !double.IsFinite(top) ||
            !double.IsFinite(rightEdge) || !double.IsFinite(bottom) ||
            rightEdge - left < 1 || bottom - top < 1)
            return false;

        bounds = new NativePresentRectangle(left, top, rightEdge, bottom);
        return true;
    }

    public static bool TryClipLineEndpointToViewport(
        NativePresentPoint start,
        NativePresentPoint target,
        int viewportWidth,
        int viewportHeight,
        out NativePresentPoint endpoint)
    {
        endpoint = default;
        if (!double.IsFinite(start.X) || !double.IsFinite(start.Y) ||
            !double.IsFinite(target.X) || !double.IsFinite(target.Y) ||
            viewportWidth <= 0 || viewportHeight <= 0 ||
            start.X < 0 || start.X > viewportWidth ||
            start.Y < 0 || start.Y > viewportHeight)
            return false;

        var dx = target.X - start.X;
        var dy = target.Y - start.Y;
        if (Math.Abs(dx) < 0.000001 && Math.Abs(dy) < 0.000001)
            return false;

        var scale = 1.0;
        if (target.X < 0)
            scale = Math.Min(scale, -start.X / dx);
        else if (target.X > viewportWidth)
            scale = Math.Min(scale, (viewportWidth - start.X) / dx);
        if (target.Y < 0)
            scale = Math.Min(scale, -start.Y / dy);
        else if (target.Y > viewportHeight)
            scale = Math.Min(scale, (viewportHeight - start.Y) / dy);
        if (!double.IsFinite(scale) || scale <= 0)
            return false;

        endpoint = new NativePresentPoint(
            Math.Clamp(start.X + dx * scale, 0, viewportWidth),
            Math.Clamp(start.Y + dy * scale, 0, viewportHeight));
        return double.IsFinite(endpoint.X) && double.IsFinite(endpoint.Y);
    }

    public static bool TryProjectSnaplineTarget(
        NativePresentCamera camera,
        NativePresentVector3 world,
        int viewportWidth,
        int viewportHeight,
        out NativePresentPoint screen)
    {
        if (TryProject(
                camera,
                world,
                viewportWidth,
                viewportHeight,
                out screen))
            return true;
        screen = default;
        if (!IsFinite(camera.Location) || !IsFinite(world) ||
            viewportWidth <= 0 || viewportHeight <= 0)
            return false;

        var (_, right, up) = GetAxes(camera.Rotation);
        var delta = world - camera.Location;
        var horizontal = Dot(delta, right);
        var vertical = Dot(delta, up);
        if (!double.IsFinite(horizontal) || !double.IsFinite(vertical))
            return false;
        if (Math.Abs(horizontal) < 0.000001 &&
            Math.Abs(vertical) < 0.000001)
            horizontal = 1;

        var normalization = Math.Max(
            Math.Max(Math.Abs(horizontal), Math.Abs(vertical)),
            1.0);
        var extent = Math.Max(viewportWidth, viewportHeight) * 2.0;
        screen = new NativePresentPoint(
            viewportWidth / 2.0 + horizontal / normalization * extent,
            viewportHeight / 2.0 - vertical / normalization * extent);
        return double.IsFinite(screen.X) && double.IsFinite(screen.Y);
    }

    public static (NativePresentVector3 Forward, NativePresentVector3 Right, NativePresentVector3 Up) GetAxes(
        NativePresentRotator rotation)
    {
        var pitch = rotation.Pitch * DegreesToRadians;
        var yaw = rotation.Yaw * DegreesToRadians;
        var roll = rotation.Roll * DegreesToRadians;
        var sp = Math.Sin(pitch);
        var cp = Math.Cos(pitch);
        var sy = Math.Sin(yaw);
        var cy = Math.Cos(yaw);
        var sr = Math.Sin(roll);
        var cr = Math.Cos(roll);
        return (
            new NativePresentVector3(cp * cy, cp * sy, sp),
            new NativePresentVector3(sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp),
            new NativePresentVector3(-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp));
    }

    private static bool IsFinite(NativePresentVector3 value) =>
        double.IsFinite(value.X) && double.IsFinite(value.Y) && double.IsFinite(value.Z);

    private static double Dot(NativePresentVector3 left, NativePresentVector3 right) =>
        left.X * right.X + left.Y * right.Y + left.Z * right.Z;
}

/// <summary>
/// Fixed-capacity, two-slot publication model. Readers receive a copy of a
/// completed slot, so a later game-thread write can never expose a partial
/// frame to a renderer that is still consuming the earlier frame.
/// </summary>
public sealed class NativePresentSnapshotBuffer<T>
{
    private readonly T[][] slots;
    private readonly object gate = new();
    private int publishedSlot = -1;
    private long sequence;

    public NativePresentSnapshotBuffer(int capacity)
    {
        if (capacity <= 0)
            throw new ArgumentOutOfRangeException(nameof(capacity));
        slots = [new T[capacity], new T[capacity]];
    }

    public NativePresentSnapshotWriter<T> BeginWrite()
    {
        lock (gate)
        {
            var slot = publishedSlot == 0 ? 1 : 0;
            return new NativePresentSnapshotWriter<T>(this, slot, slots[slot]);
        }
    }

    public void Publish(NativePresentSnapshotWriter<T> writer)
    {
        ArgumentNullException.ThrowIfNull(writer);
        lock (gate)
        {
            writer.ValidateOwner(this);
            writer.MarkPublished();
            publishedSlot = writer.Slot;
            ++sequence;
        }
    }

    public bool TryRead(out NativePresentSnapshot<T> frame)
    {
        lock (gate)
        {
            if (publishedSlot < 0)
            {
                frame = default;
                return false;
            }
            var copy = new T[slots[publishedSlot].Length];
            Array.Copy(slots[publishedSlot], copy, copy.Length);
            frame = new NativePresentSnapshot<T>(sequence, copy.AsMemory(0, writerCounts[publishedSlot]));
            return true;
        }
    }

    private readonly int[] writerCounts = new int[2];

    internal void SetCount(int slot, int count) => writerCounts[slot] = count;
}

public readonly record struct NativePresentSnapshot<T>(long Sequence, ReadOnlyMemory<T> Items);

public sealed class NativePresentSnapshotWriter<T>
{
    private readonly NativePresentSnapshotBuffer<T> owner;
    private readonly T[] items;
    private bool published;
    private int count;

    internal NativePresentSnapshotWriter(NativePresentSnapshotBuffer<T> owner, int slot, T[] items)
    {
        this.owner = owner;
        Slot = slot;
        this.items = items;
    }

    internal int Slot { get; }

    public void Add(T item)
    {
        if (published)
            throw new InvalidOperationException("A published native ESP frame is immutable.");
        if (count >= items.Length)
            throw new InvalidOperationException("Native ESP snapshot capacity exceeded.");
        items[count++] = item;
    }

    internal void ValidateOwner(NativePresentSnapshotBuffer<T> expected)
    {
        if (!ReferenceEquals(owner, expected) || published)
            throw new InvalidOperationException("The native ESP frame cannot be published by this buffer.");
    }

    internal void MarkPublished()
    {
        owner.SetCount(Slot, count);
        published = true;
    }
}
