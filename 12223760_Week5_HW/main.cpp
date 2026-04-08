#pragma comment(linker, "/entry:WinMainCRTStartup /subsystem:console")

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

struct VideoConfig {
    int Width = 800;
    int Height = 600;
    bool IsFullscreen = false;
    bool NeedsResize = false;
    int VSync = 1;
} g_Config;

ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pImmediateContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_pRenderTargetView = nullptr;
ID3D11VertexShader* g_pVertexShader = nullptr;
ID3D11PixelShader* g_pPixelShader = nullptr;
ID3D11InputLayout* g_pInputLayout = nullptr;

struct Vertex {
    float x, y, z;
    float r, g, b, a;
};

void RebuildVideoResources(HWND hWnd) {
    if (!g_pSwapChain) return;

    if (g_pRenderTargetView) {
        g_pRenderTargetView->Release();
        g_pRenderTargetView = nullptr;
    }

    g_pSwapChain->ResizeBuffers(0, g_Config.Width, g_Config.Height, DXGI_FORMAT_UNKNOWN, 0);

    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);
    if (pBackBuffer == nullptr) {
        printf("GETBUFFER ERROR\n");
        return;
    }
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_pRenderTargetView);
    pBackBuffer->Release();

    if (!g_Config.IsFullscreen) {
        RECT rc = { 0, 0, g_Config.Width, g_Config.Height };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hWnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
    }

    g_Config.NeedsResize = false;
    printf("[Video] Changed: %d x %d\n", g_Config.Width, g_Config.Height);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_DESTROY) {
        PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

// =======================================================================
// 엔진 핵심 구조 (Component & GameObject)
// =======================================================================
class Component {
public:
    class GameObject* pOwner = nullptr;
    bool isStarted = 0;

    virtual void Start() = 0;
    virtual void Input() {}
    virtual void Update(float dt) = 0;
    virtual void Render() {}

    virtual ~Component() {}
};

class GameObject {
public:
    std::vector<Component*> components;
    std::string name;

    GameObject(std::string n) {
        name = n;
    }

    ~GameObject() {
        for (int i = 0; i < (int)components.size(); i++) {
            delete components[i];
        }
    }

    void AddComponent(Component* pComp) {
        pComp->pOwner = this;
        pComp->isStarted = false;
        components.push_back(pComp);
    }
};

// =======================================================================
// 과제 요구사항: 플레이어 조작 컴포넌트
// =======================================================================
class PlayerControl : public Component {
public:
    float x, y, speed, R, G, B;
    int playertype;
    bool moveUp, moveDown, moveLeft, moveRight;

    // 컴포넌트별 개별 정점 버퍼 (메모리 누수 방지)
    ID3D11Buffer* pVertexBuffer = nullptr;

    PlayerControl(int pt) {
        this->playertype = pt;
    }

    ~PlayerControl() {
        if (pVertexBuffer) pVertexBuffer->Release();
    }

    void Start() override {
        if (this->playertype == 1) { // Player 1
            x = 0.3f; y = 0.3f;
            this->R = 1.0f; this->G = 0.0f; this->B = 0.0f; // Red
        }
        else if (this->playertype == 2) { // Player 2
            x = -0.3f; y = -0.3f;
            this->R = 0.0f; this->G = 1.0f; this->B = 0.0f; // Green
        }

        speed = 1.5f; // DX11 NDC 좌표계(-1.0 ~ 1.0)에 맞춘 속도
        moveUp = moveDown = moveLeft = moveRight = false;

        // [최적화] 동적 버퍼를 Start에서 1회만 생성
        Vertex vertices[3] = {};
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.ByteWidth = sizeof(Vertex) * 3;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        D3D11_SUBRESOURCE_DATA initData = { vertices, 0, 0 };
        g_pd3dDevice->CreateBuffer(&bd, &initData, &pVertexBuffer);
    }

    void Input() override {
        if (this->playertype == 1) {
            moveUp = (GetAsyncKeyState(VK_UP) & 0x8000);
            moveDown = (GetAsyncKeyState(VK_DOWN) & 0x8000);
            moveLeft = (GetAsyncKeyState(VK_LEFT) & 0x8000);
            moveRight = (GetAsyncKeyState(VK_RIGHT) & 0x8000);
        }
        else if (this->playertype == 2) {
            moveUp = (GetAsyncKeyState('W') & 0x8000);
            moveDown = (GetAsyncKeyState('S') & 0x8000);
            moveLeft = (GetAsyncKeyState('A') & 0x8000);
            moveRight = (GetAsyncKeyState('D') & 0x8000);
        }
    }

    void Update(float dt) override {
        if (moveUp)    y += speed * dt;
        if (moveDown)  y -= speed * dt;
        if (moveLeft)  x -= speed * dt;
        if (moveRight) x += speed * dt;
    }

    void Render() override {
        if (!pVertexBuffer) return;

        // 매 프레임 정점 위치 갱신 (Map/Unmap)
        D3D11_MAPPED_SUBRESOURCE mappedResource;
        g_pImmediateContext->Map(pVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);

        Vertex* vertices = (Vertex*)mappedResource.pData;
        vertices[0] = { x + 0.0f, y + 0.1f, 0.5f, R, G, B, 1.0f };
        vertices[1] = { x + 0.1f, y - 0.1f, 0.5f, R, G, B, 1.0f };
        vertices[2] = { x - 0.1f, y - 0.1f, 0.5f, R, G, B, 1.0f };

        g_pImmediateContext->Unmap(pVertexBuffer, 0);

        // 그리기 명령
        UINT stride = sizeof(Vertex), offset = 0;
        g_pImmediateContext->IASetVertexBuffers(0, 1, &pVertexBuffer, &stride, &offset);
        g_pImmediateContext->Draw(3, 0);
    }
};

// =======================================================================
// 게임 루프 매니저
// =======================================================================
class GameLoop {
public:
    std::vector<GameObject*> gameWorld;

    ~GameLoop() {
        for (int i = 0; i < (int)gameWorld.size(); i++) {
            delete gameWorld[i];
        }
    }

    void Input() {
        for (int i = 0; i < (int)gameWorld.size(); i++) {
            for (int j = 0; j < (int)gameWorld[i]->components.size(); j++) {
                gameWorld[i]->components[j]->Input();
            }
        }
    }

    void Update(float dt) {
        for (int i = 0; i < (int)gameWorld.size(); i++) {
            for (int j = 0; j < (int)gameWorld[i]->components.size(); j++) {
                if (gameWorld[i]->components[j]->isStarted == false) {
                    gameWorld[i]->components[j]->Start();
                    gameWorld[i]->components[j]->isStarted = true;
                }
                gameWorld[i]->components[j]->Update(dt);
            }
        }
    }

    void Render() {
        for (int i = 0; i < (int)gameWorld.size(); i++) {
            for (int j = 0; j < (int)gameWorld[i]->components.size(); j++) {
                gameWorld[i]->components[j]->Render();
            }
        }
    }
};

// =======================================================================
// 메인 함수
// =======================================================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    // 1. 윈도우 클래스 등록 및 생성
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEX) };
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.lpszClassName = L"DX11VideoClass";
    RegisterClassExW(&wcex);

    RECT rc = { 0, 0, g_Config.Width, g_Config.Height };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hWnd = CreateWindowW(L"DX11VideoClass", L"DirectX 11 Component Engine",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInstance, nullptr);
    ShowWindow(hWnd, nCmdShow);

    // 2. DX11 Device & SwapChain 생성
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = g_Config.Width;
    sd.BufferDesc.Height = g_Config.Height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;

    D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, nullptr, &g_pImmediateContext);

    // 3. 초기 리소스 빌드
    RebuildVideoResources(hWnd);

    // 4. 셰이더 컴파일
    const char* shaderSource = R"(
        struct VS_IN { float3 pos : POSITION; float4 col : COLOR; };
        struct PS_IN { float4 pos : SV_POSITION; float4 col : COLOR; };
        PS_IN VS(VS_IN input) { PS_IN output; output.pos = float4(input.pos, 1.0f); output.col = input.col; return output; }
        float4 PS(PS_IN input) : SV_Target { return input.col; }
    )";
    ID3DBlob* vsBlob, * psBlob;
    D3DCompile(shaderSource, strlen(shaderSource), nullptr, nullptr, nullptr, "VS", "vs_4_0", 0, 0, &vsBlob, nullptr);
    D3DCompile(shaderSource, strlen(shaderSource), nullptr, nullptr, nullptr, "PS", "ps_4_0", 0, 0, &psBlob, nullptr);
    g_pd3dDevice->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_pVertexShader);
    g_pd3dDevice->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_pPixelShader);

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    g_pd3dDevice->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_pInputLayout);
    vsBlob->Release(); psBlob->Release();


    // --- [게임 월드 및 객체 초기화] ---
    GameLoop gLoop;

    GameObject* player1 = new GameObject("Player1");
    player1->AddComponent(new PlayerControl(1));
    gLoop.gameWorld.push_back(player1);

    GameObject* player2 = new GameObject("Player2");
    player2->AddComponent(new PlayerControl(2));
    gLoop.gameWorld.push_back(player2);

    auto prevTime = std::chrono::high_resolution_clock::now();

    // --- [메인 게임 루프] ---
    MSG msg = { 0 };
    while (WM_QUIT != msg.message) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else {
            // [시스템 제어 요구사항: ESC 종료]
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) break;

            // [창 모드 및 전체화면 제어]
            if (GetAsyncKeyState('F') & 0x0001) {
                g_Config.IsFullscreen = !g_Config.IsFullscreen;
                g_pSwapChain->SetFullscreenState(g_Config.IsFullscreen, nullptr);
            }
            if (GetAsyncKeyState('1') & 0x0001) { g_Config.Width = 800; g_Config.Height = 600; g_Config.NeedsResize = true; }
            if (GetAsyncKeyState('2') & 0x0001) { g_Config.Width = 1280; g_Config.Height = 720; g_Config.NeedsResize = true; }
            if (g_Config.NeedsResize) RebuildVideoResources(hWnd);

            // [A. 프레임 독립적 이동 (Delta Time 계산)]
            auto currentTime = std::chrono::high_resolution_clock::now();
            std::chrono::duration<float> elapsed = currentTime - prevTime;
            float dt = elapsed.count();
            prevTime = currentTime;

            // [B, C, D. 입력 및 업데이트]
            gLoop.Input();
            gLoop.Update(dt);

            // [E. 렌더링 파이프라인 설정]
            float clearColor[] = { 0.1f, 0.2f, 0.3f, 1.0f };
            g_pImmediateContext->ClearRenderTargetView(g_pRenderTargetView, clearColor);

            D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)g_Config.Width, (float)g_Config.Height, 0.0f, 1.0f };
            g_pImmediateContext->RSSetViewports(1, &vp);
            g_pImmediateContext->OMSetRenderTargets(1, &g_pRenderTargetView, nullptr);

            g_pImmediateContext->IASetInputLayout(g_pInputLayout);
            g_pImmediateContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_pImmediateContext->VSSetShader(g_pVertexShader, nullptr, 0);
            g_pImmediateContext->PSSetShader(g_pPixelShader, nullptr, 0);

            // 컴포넌트 내부(PlayerControl)에서 개별적으로 Draw() 호출
            gLoop.Render();

            g_pSwapChain->Present(g_Config.VSync, 0);
        }
    }

    // [자원 정리]
    if (g_pInputLayout) g_pInputLayout->Release();
    if (g_pVertexShader) g_pVertexShader->Release();
    if (g_pPixelShader) g_pPixelShader->Release();
    if (g_pRenderTargetView) g_pRenderTargetView->Release();
    if (g_pSwapChain) g_pSwapChain->Release();
    if (g_pImmediateContext) g_pImmediateContext->Release();
    if (g_pd3dDevice) g_pd3dDevice->Release();

    return (int)msg.wParam;
}