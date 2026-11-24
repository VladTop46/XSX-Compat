RWStructuredBuffer<uint> outputBuffer : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint x = tid.x;
    x = x * 1664525u + 1013904223u;
    x ^= x >> 16;
    x *= 2246822519u;
    x ^= x >> 13;

    outputBuffer[tid.x] = x;
}
