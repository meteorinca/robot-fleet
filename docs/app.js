document.addEventListener('DOMContentLoaded', () => {
    fetchFleetData();

    // Setup Search
    const searchInput = document.getElementById('search');
    searchInput.addEventListener('input', filterBots);
});

async function fetchFleetData() {
    const grid = document.getElementById('fleet-grid');
    
    try {
        // Fetch the markdown file
        // Appending a cache-buster query parameter ensures we get the latest push
        const response = await fetch('fleet.md?t=' + new Date().getTime());
        
        if (!response.ok) {
            throw new Error(`Failed to load fleet.md: ${response.status}`);
        }
        
        const text = await response.text();
        document.getElementById('raw-md').textContent = text;
        
        // Parse the Markdown table
        const bots = parseMarkdownTable(text);
        
        // Update UI
        renderBots(bots);
        updateStats(bots);
        
        document.getElementById('subtitle').textContent = 'Telemetry sync complete.';
        
    } catch (error) {
        grid.innerHTML = `
            <div class="loading-state">
                <i class="ri-error-warning-line" style="font-size: 3rem; color: var(--status-offline); margin-bottom: 1rem;"></i>
                <p>Error loading fleet data: ${error.message}</p>
                <p style="font-size: 0.85rem; margin-top: 0.5rem;">Ensure fleet.md exists and is served correctly.</p>
            </div>
        `;
        document.getElementById('subtitle').textContent = 'System offline.';
    }
}

function parseMarkdownTable(md) {
    const bots = [];
    const lines = md.split('\n');
    
    let isTable = false;
    
    for (let line of lines) {
        line = line.trim();
        
        // Identify table start
        if (line.startsWith('|') && line.includes('Bot Name')) {
            isTable = true;
            continue;
        }
        
        // Skip separator line
        if (isTable && line.includes('---')) {
            continue;
        }
        
        // Parse row
        if (isTable && line.startsWith('|')) {
            // Split by pipe and clean up whitespace
            const parts = line.split('|').map(s => s.trim()).filter(s => s);
            
            if (parts.length >= 4) {
                bots.push({
                    name: parts[0],
                    platform: parts[1],
                    version: parts[2],
                    status: parts[3]
                });
            }
        }
    }
    
    return bots;
}

function renderBots(bots) {
    const grid = document.getElementById('fleet-grid');
    grid.innerHTML = '';
    
    if (bots.length === 0) {
        grid.innerHTML = `
            <div class="loading-state">
                <p>No bots found in fleet.md.</p>
            </div>
        `;
        return;
    }
    
    bots.forEach(bot => {
        const card = document.createElement('div');
        card.className = 'bot-card';
        card.dataset.name = bot.name.toLowerCase();
        card.dataset.platform = bot.platform.toLowerCase();
        
        // Determine status class
        let statusClass = 'status-offline';
        let s = bot.status.toLowerCase();
        if (s.includes('active') || s.includes('online') || s.includes('ok')) statusClass = 'status-active';
        else if (s.includes('dev') || s.includes('test')) statusClass = 'status-dev';
        
        // Determine icon based on platform
        let icon = 'ri-robot-line';
        if (bot.platform.includes('dog')) icon = 'ri-aliens-line'; // fun alien dog
        if (bot.platform.includes('rf')) icon = 'ri-broadcast-line';
        
        card.innerHTML = `
            <div class="card-header">
                <div>
                    <h3 class="card-title">${bot.name}</h3>
                    <div class="card-platform"><i class="ri-cpu-line"></i> ${bot.platform}</div>
                </div>
                <i class="${icon} bot-icon"></i>
            </div>
            
            <div style="margin-top: 1rem; margin-bottom: 2rem;">
                <span class="status-badge ${statusClass}">${bot.status}</span>
            </div>
            
            <div class="version-tag">
                <i class="ri-git-branch-line" style="font-size: 0.9em; opacity: 0.7;"></i> 
                ${bot.version}
            </div>
        `;
        
        grid.appendChild(card);
    });
}

function updateStats(bots) {
    document.getElementById('total-bots').textContent = bots.length;
    
    const active = bots.filter(b => {
        const s = b.status.toLowerCase();
        return s.includes('active') || s.includes('online') || s.includes('ok');
    }).length;
    
    document.getElementById('active-bots').textContent = active;
}

function filterBots(e) {
    const term = e.target.value.toLowerCase();
    const cards = document.querySelectorAll('.bot-card');
    
    cards.forEach(card => {
        const name = card.dataset.name;
        const platform = card.dataset.platform;
        
        if (name.includes(term) || platform.includes(term)) {
            card.style.display = 'block';
        } else {
            card.style.display = 'none';
        }
    });
}
