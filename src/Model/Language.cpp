// 移植自 BIMCraft crates/core/src/language.rs 与 translations/project/{pt,es}.po（f3bbb33），MIT 许可，见 THIRD_PARTY_NOTICES.md
#include "Model/Language.h"
#include "Model/Text.h"

namespace MiniBIM::Bim::LanguageOf
{
    namespace
    {
        struct Words
        {
            const char* english;
            const char* portuguese;
            const char* spanish;
        };

        // 用到的词（新项目的名称、标高、房间、材质、内置族）。BIMCraft 的项目词典有几百条，移植图纸、明细表时再补
        constexpr Words kWords[] = {
            { "Project",        "Projeto",           "Proyecto" },
            { "Level",          "Nível",             "Nivel" },
            { "Room",           "Ambiente",          "Habitación" },
            { "Brick",          "Tijolo",            "Ladrillo" },
            { "Insulation",     "Isolamento",        "Aislamiento" },
            { "Concrete block", "Bloco de concreto", "Bloque de hormigón" },
            { "Plaster",        "Reboco",            "Enlucido" },
            { "Concrete",       "Concreto",          "Hormigón" },
            { "Screed",         "Contrapiso",        "Recrecido" },
            { "Timber",         "Madeira",           "Madera" },
            { "Gypsum board",   "Placa de gesso",    "Placa de yeso" },
            { "Ceramic tile",   "Cerâmica",          "Baldosa cerámica" },
            { "Roof tiles",     "Telhas",            "Tejas" },
            { "Glass",          "Vidro",             "Vidrio" },
            { "Copper",         "Cobre",             "Cobre" },
            // 第 2c 步：内置族的名称与参数名（family.rs、fixtures.rs）
            { "Width", "Largura", "Ancho" },
            { "Depth", "Profundidade", "Profundidad" },
            { "Height", "Altura", "Altura" },
            { "Length", "Comprimento", "Longitud" },
            { "Thickness", "Espessura", "Espesor" },
            { "Leg Width", "Largura da perna", "Ancho de la pata" },
            { "Seat Height", "Altura do assento", "Altura del asiento" },
            { "Material", "Material", "Material" },
            { "Fabric", "Tecido", "Tela" },
            { "Porcelain", "Porcelana", "Porcelana" },
            { "Table", "Mesa", "Mesa" },
            { "Chair", "Cadeira", "Silla" },
            { "Headboard Height", "Altura da cabeceira", "Altura del cabecero" },
            { "Bed", "Cama", "Cama" },
            { "Arm Width", "Largura do braço", "Ancho del brazo" },
            { "Sofa", "Sofá", "Sofá" },
            { "Desk", "Escrivaninha", "Escritorio" },
            { "Wardrobe", "Guarda-roupa", "Armario" },
            { "Worktop", "Bancada", "Encimera" },
            { "Base Cabinet", "Armário inferior", "Mueble bajo" },
            { "WC", "Vaso sanitário", "Inodoro" },
            { "Washbasin", "Lavatório", "Lavabo" },
            { "Bathtub", "Banheira", "Bañera" },
            { "Frame Width", "Largura do batente", "Ancho del marco" },
            { "Frame Depth", "Profundidade do batente", "Profundidad del marco" },
            { "Leaf Thickness", "Espessura da folha", "Espesor de la hoja" },
            { "Frame Material", "Material do batente", "Material del marco" },
            { "Leaf Material", "Material da folha", "Material de la hoja" },
            { "Metal", "Metal", "Metal" },
            { "Single Door", "Porta simples", "Puerta simple" },
            { "Double Door", "Porta dupla", "Puerta doble" },
            { "Sliding Door", "Porta de correr", "Puerta corredera" },
            { "Fixed Window", "Janela fixa", "Ventana fija" },
            { "Casement Window", "Janela de abrir", "Ventana abatible" },
            { "Sliding Window", "Janela de correr", "Ventana corredera" },
            { "Kitchen Sink", "Pia de cozinha", "Fregadero de cocina" },
            { "Shower", "Chuveiro", "Ducha" },
            { "Floor Drain", "Ralo", "Sumidero de piso" },
            { "Diameter", "Diâmetro", "Diámetro" },
            { "PVC", "PVC", "PVC" },
            { "Elbow 90", "Joelho 90", "Codo 90" },
            { "Tee", "Tê", "Te" },
            { "Gate Valve", "Registro de gaveta", "Válvula de compuerta" },
            { "Mounting Height", "Altura de instalação", "Altura de montaje" },
            { "Distribution Board", "Quadro de distribuição", "Cuadro de distribución" },
            { "Ceiling Luminaire", "Luminária de teto", "Luminaria de techo" },
            { "Recessed Luminaire", "Luminária embutida", "Luminaria empotrada" },
            { "Outlet", "Tomada", "Tomacorriente" },
            { "Switch", "Interruptor", "Interruptor" },
            { "Air Terminal", "Captor", "Terminal aéreo" },
            { "Earthing Electrode", "Haste de aterramento", "Electrodo de puesta a tierra" },
            { "Inspection Box", "Caixa de inspeção", "Caja de inspección" },
            { "Data Outlet", "Ponto de dados", "Punto de datos" },
            { "Voice Outlet", "Ponto de voz", "Punto de voz" },
            { "Patch Panel", "Patch panel", "Panel de parcheo" },
            { "Rack", "Rack", "Rack" },
            // 第 5d 步：线型图案、线样式、填充图案、填充区域类型（lines.rs、fills.rs）
            { "Hairline", "Linha capilar", "Línea capilar" },
            { "Thin Lines", "Linhas finas", "Líneas finas" },
            { "Medium Lines", "Linhas médias", "Líneas medias" },
            { "Wide Lines", "Linhas grossas", "Líneas gruesas" },
            { "Hidden Lines", "Linhas ocultas", "Líneas ocultas" },
            { "Centre Lines", "Linhas de eixo", "Líneas de eje" },
            { "Overhead Lines", "Linhas superiores", "Líneas superiores" },
            { "Dash", "Tracejado", "Trazos" },
            { "Long Dash", "Traço longo", "Trazo largo" },
            { "Dot", "Pontilhado", "Puntos" },
            { "Dash Dot", "Traço e ponto", "Trazo y punto" },
            { "Hidden", "Oculta", "Oculta" },
            { "Centre", "Eixo", "Eje" },
            { "Solid fill", "Preenchimento sólido", "Relleno sólido" },
            { "Diagonal Up", "Diagonal para cima", "Diagonal ascendente" },
            { "Diagonal Down", "Diagonal para baixo", "Diagonal descendente" },
            { "Diagonal Crosshatch", "Hachura cruzada diagonal", "Sombreado cruzado diagonal" },
            { "Crosshatch", "Hachura cruzada", "Sombreado cruzado" },
            { "Horizontal", "Horizontal", "Horizontal" },
            { "Vertical", "Vertical", "Vertical" },
            { "Sand", "Areia", "Arena" },
            { "Brick 225 x 75", "Tijolo 225 x 75", "Ladrillo 225 x 75" },
            { "Tile 300", "Ladrilho 300", "Baldosa 300" },
            { "Solid Black", "Preto sólido", "Negro sólido" },
            { "Masking", "Máscara", "Máscara" },
            { "Copy", "Cópia", "Copia" },
            { "Dependent", "Dependente", "Dependiente" },
            // 第 5e2 步：内置明细表的名称与开启方式（schedule.rs）
            { "Room Schedule", "Tabela de ambientes", "Tabla de habitaciones" },
            { "Door Schedule", "Tabela de portas", "Tabla de puertas" },
            { "Window Schedule", "Tabela de janelas", "Tabla de ventanas" },
            { "Wall Schedule", "Tabela de paredes", "Tabla de muros" },
            { "Floor Schedule", "Tabela de pisos", "Tabla de pisos" },
            { "Level Schedule", "Tabela de níveis", "Tabla de niveles" },
            { "Swing", "De abrir", "Batiente" },
            { "Double Swing", "De abrir, duas folhas", "Batiente doble" },
            { "Sliding", "De correr", "Corredera" },
            { "Double Sliding", "De correr, duas folhas", "Corredera doble" },
            { "Folding", "Camarão", "Plegable" },
            { "Fixed", "Fixa", "Fija" },
            // 第 5e3 步：剖面与立面的名称（model.rs 的 Facing）
            { "Section", "Corte", "Sección" },
            { "Elevation", "Elevação", "Alzado" },
            { "North", "Norte", "Norte" },
            { "South", "Sul", "Sur" },
            { "East", "Leste", "Este" },
            { "West", "Oeste", "Oeste" },
            // 第 5e4 步：图纸（标题栏、图纸上明细表的标题与格子，sheet.rs 与 schedule.rs）
            { "Unnamed", "Sem nome", "Sin nombre" },
            { "SCALE", "ESCALA", "ESCALA" },
            { "SCALE AS INDICATED", "ESCALA CONFORME INDICADO", "ESCALA SEGÚN SE INDICA" },
            { "Total", "Total", "Total" },
            { "Yes", "Sim", "Sí" },
            { "No", "Não", "No" },
            { "Id", "Id", "Id" },
            { "Name", "Nome", "Nombre" },
            { "Number", "Número", "Número" },
            { "Type", "Tipo", "Tipo" },
            { "Family", "Família", "Familia" },
            { "Mark", "Marca", "Marca" },
            { "Area", "Área", "Área" },
            { "Perimeter", "Perímetro", "Perímetro" },
            { "Volume", "Volume", "Volumen" },
            { "Count", "Contagem", "Recuento" },
            { "Offset", "Deslocamento", "Desfase" },
            { "Operation", "Abertura", "Apertura" },
            { "Sill", "Peitoril", "Alféizar" },
            { "Slope", "Inclinação", "Pendiente" },
            { "Plan Area", "Área em planta", "Área en planta" },
            { "Structural", "Estrutural", "Estructural" },
            { "System", "Sistema", "Sistema" },
            { "Wall", "Parede", "Muro" },
        };
    }

    const char* Code(Language l)
    {
        switch (l)
        {
        case Language::English:    return "en";
        case Language::Portuguese: return "pt";
        case Language::Spanish:    return "es";
        }
        return "en";
    }

    const char* Label(Language l)
    {
        switch (l)
        {
        case Language::English:    return "English";
        case Language::Portuguese: return "Português (Brasil)";
        case Language::Spanish:    return "Español";
        }
        return "English";
    }

    std::optional<Language> FromCode(std::string_view code)
    {
        const std::string lower = Text::AsciiLower(Text::Trim(code));
        const std::string first = lower.substr(0, lower.find_first_of("-_."));
        if (first == "en" || first == "english")
            return Language::English;
        // "português"、"español" 的非 ASCII 字母 AsciiLower 不动，开头大写的照样认
        if (first == "pt" || first == "portuguese" || first == "português" || first == "portuguÊs")
            return Language::Portuguese;
        if (first == "es" || first == "spanish" || first == "español" || first == "espaÑol")
            return Language::Spanish;
        return std::nullopt;
    }

    std::string Word(Language l, std::string_view english)
    {
        if (l == Language::English)
            return std::string(english);
        for (const Words& w : kWords)
            if (english == w.english)
                return l == Language::Portuguese ? w.portuguese : w.spanish;
        return std::string(english);
    }
}
