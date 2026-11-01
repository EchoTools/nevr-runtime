// SAPIWrapper<NRadEngine::NRadGame::NR15RestAPI::SSessionRequest>::Execute
// Quest libr15.so VA 0x13fe8c4 (RVA 0x12fe8c4); project-wide PC search found no matching wrapper.
// Search: `SSessionRequest::Execute`, searched all 1,041 binaries in project echovr.
// ReVault binary 576, raw parent 100226344, reconstruction node 100600197; tier: reconstructed; grade: M.
// @0x13fe8c4 — NRadEngine::NRadGame::NR15RestAPI::SAPIWrapper<SSessionRequest>::Execute
/* @confidence: M */
#include <cstddef>
#include <cstdint>

void Execute(CHttpListenerRequest* request,
             SAPIWrapperContext* context) {
    CAllocator* allocator = CMemoryContext::CurrentPtr();
    CMemBlock errorMessage(0, 0, allocator);
    CJson response;

    const auto* api = *reinterpret_cast<const ApiState* const*>(
        reinterpret_cast<const std::byte*>(context) + 0x08);
    const int requestKind = *reinterpret_cast<const int*>(context);
    const auto* game = *reinterpret_cast<const GameState* const*>(
        reinterpret_cast<const std::byte*>(context) + 0x40);
    const bool featureEnabled =
        ((*reinterpret_cast<const std::uint8_t*>(
              reinterpret_cast<const std::byte*>(api) + 0x8c09) >> 3) & 1) != 0;
    bool delegatedRequest = false;

    if (featureEnabled && requestKind == 9) {
        const auto gameType = game != nullptr ? game->TypeHash() : 0;
        const bool restrictedSocialType =
            game != nullptr &&
            (gameType == 0x42d9cf9cfddcf76ULL ||
             gameType == 0x305d6e37c1589c45ULL ||
             gameType == 0x163956e6e431e617ULL);

        if (!restrictedSocialType) {
            delegatedRequest = true;
            CJson body;
            const int bodyResult = request->Body(body);
            if (bodyResult == 0) {
                // Raw stack layout passed to SSessionRequest::Execute:
                // +0x00=context+0x2b08, +0x08=context+0x2b10,
                // +0x10=*(context+0x08), +0x18=context, +0x20=context+0x2aa8.
                SR15APIContext apiContext{
                    *reinterpret_cast<const void* const*>(
                        reinterpret_cast<const std::byte*>(context) + 0x2b08),
                    *reinterpret_cast<const void* const*>(
                        reinterpret_cast<const std::byte*>(context) + 0x2b10),
                    api,
                    context,
                    *reinterpret_cast<const void* const*>(
                        reinterpret_cast<const std::byte*>(context) + 0x2aa8)};
                SSessionRequest::Execute(apiContext, body, response);
            } else {
                CMemBlock parseText(0, 0, CMemoryContext::CurrentPtr());
                if (ExtractErrorMessage(bodyResult, true, true, parseText) != 0) {
                    errorMessage += "Parsing error on json body parameters: ";
                    errorMessage += parseText;
                }
                // The raw body constructs this diagnostic without adding it to the JSON.
            }
        }
    }

    // Accepted /session requests return delegated JSON with its status field.
    // Wrapper-level refusals additionally set err_description.
    response.SetInt(0x02cb20fc);
    if (!delegatedRequest) {
        response.SetString("err_description");
    }
    request->ReplyWithJson(response);
}
