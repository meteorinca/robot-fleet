document.addEventListener('DOMContentLoaded', () => {
    fetchFleetData();

    // Setup Search
    const searchInput = document.getElementById('search');
    searchInput.addEventListener('input', filterBots);
});

async function fetchFleetData() {
    const grid = document.getElementById('fleet-grid');
    
    try {
        // Fetch YAML and CSV concurrently
        const yamlPromise = fetch('fleet.yaml?t=' + new Date().getTime()).then(r => {
            if (!r.ok) throw new Error(`Failed to load fleet.yaml: ${r.status}`);
            return r.text();
        });
        
        // CSV might not exist yet if pinginthembots.py hasn't run
        const csvPromise = fetch('ping_log.csv?t=' + new Date().getTime()).then(r => {
            if (!r.ok) return '';
            return r.text();
        }).catch(() => '');

        const [yamlText, csvText] = await Promise.all([yamlPromise, csvPromise]);
        
        // Show raw data in viewer (optional)
        document.getElementById('raw-md').textContent = yamlText + '\n\n' + csvText;
        
        // Parse YAML
        const fleetData = jsyaml.load(yamlText);
        
        // Parse CSV
        const pingLog = {};
        if (csvText.trim()) {
            Papa.parse(csvText, {
                header: true,
                skipEmptyLines: true,
                complete: function(results) {
                    results.data.forEach(row => {
                        // Keep latest ping by BotName (it appends so the last row is the latest for that bot)
                        if (row.BotName && row.Status) {
                            pingLog[row.BotName] = {
                                status: row.Status,
                                ip: row.IPAddress,
                                time: row.Timestamp
                            };
                        }
                    });
                }
            });
        }
        
        // Merge data
        const bots = [];
        
        if (fleetData.active_fleet) {
            fleetData.active_fleet.forEach(bot => {
                const log = pingLog[bot.hostname] || null;
                bots.push({
                    type: 'active',
                    name: bot.name,
                    hostname: bot.hostname,
                    platform: bot.platform,
                    version: bot.version,
                    status: bot.status,
                    notes: bot.notes,
                    ping: log
                });
            });
        }
        
        if (fleetData.shipped_bots) {
            fleetData.shipped_bots.forEach(bot => {
                const log = pingLog[bot.hostname] || null;
                bots.push({
                    type: 'shipped',
                    name: bot.hostname, // Shipped bots use hostname as identifier mostly
                    hostname: bot.hostname,
                    platform: 'Shipped',
                    version: bot.firmware_version,
                    customer: bot.customer,
                    progress: bot.course_progress,
                    notes: bot.notes,
                    ping: log
                });
            });
        }
        
        // Update UI
        renderBots(bots);
        updateStats(bots);
        
        document.getElementById('subtitle').textContent = 'Telemetry sync complete.';
        
    } catch (error) {
        grid.innerHTML = `
            <div class="loading-state">
                <i class="ri-error-warning-line" style="font-size: 3rem; color: var(--status-offline); margin-bottom: 1rem;"></i>
                <p>Error loading fleet data: ${error.message}</p>
                <p style="font-size: 0.85rem; margin-top: 0.5rem;">Ensure fleet.yaml exists and is served correctly.</p>
            </div>
        `;
        document.getElementById('subtitle').textContent = 'System offline.';
    }
}

function renderBots(bots) {
    const grid = document.getElementById('fleet-grid');
    grid.innerHTML = '';
    
    if (bots.length === 0) {
        grid.innerHTML = `
            <div class="loading-state">
                <p>No bots found in fleet.yaml.</p>
            </div>
        `;
        return;
    }
    
    bots.forEach(bot => {
        const card = document.createElement('div');
        card.className = `bot-card ${bot.type === 'shipped' ? 'shipped-card' : ''}`;
        card.dataset.name = (bot.name || '').toLowerCase();
        card.dataset.platform = (bot.platform || '').toLowerCase();
        card.dataset.customer = (bot.customer || '').toLowerCase();
        card.dataset.hostname = (bot.hostname || '').toLowerCase();
        
        // Determine status class
        let statusClass = 'status-offline';
        let statusText = bot.status || 'Unknown';
        
        if (bot.type === 'shipped') {
            statusClass = 'status-shipped';
            statusText = 'Shipped';
        } else {
            let s = statusText.toLowerCase();
            if (s.includes('active') || s.includes('online') || s.includes('ok')) statusClass = 'status-active';
            else if (s.includes('dev') || s.includes('test')) statusClass = 'status-dev';
        }
        
        // Determine icon based on platform
        let icon = 'ri-robot-line';
        if (bot.platform && bot.platform.toLowerCase().includes('dog')) icon = 'ri-aliens-line'; 
        if (bot.platform && bot.platform.toLowerCase().includes('rf')) icon = 'ri-broadcast-line';
        if (bot.type === 'shipped') icon = 'ri-truck-line';
        
        let extraDetails = '';
        if (bot.type === 'shipped') {
            extraDetails = `
                <div class="detail-row"><i class="ri-user-line"></i> <strong>Customer:</strong> ${bot.customer || 'N/A'}</div>
                <div class="detail-row"><i class="ri-book-read-line"></i> <strong>Progress:</strong> ${bot.progress || 'N/A'}</div>
                <div class="detail-row"><i class="ri-sticky-note-line"></i> <strong>Notes:</strong> ${bot.notes || 'N/A'}</div>
            `;
        } else {
            extraDetails = `
                <div class="detail-row"><i class="ri-sticky-note-line"></i> <strong>Notes:</strong> ${bot.notes || 'N/A'}</div>
            `;
        }
        
        let pingDetails = '';
        if (bot.ping) {
            let pClass = bot.ping.status.toLowerCase() === 'up' ? 'ping-up' : 'ping-down';
            let iconClass = bot.ping.status.toLowerCase() === 'up' ? 'ri-wifi-line' : 'ri-wifi-off-line';
            pingDetails = `
                <div class="ping-stats ${pClass}">
                    <span><i class="${iconClass}"></i> Network: ${bot.ping.status}</span>
                    <span style="font-size: 0.8em; opacity: 0.8">${bot.ping.ip}</span>
                </div>
            `;
        } else {
            pingDetails = `
                <div class="ping-stats ping-unknown">
                    <span><i class="ri-question-mark"></i> Network: Unknown</span>
                </div>
            `;
        }

        card.innerHTML = `
            <div class="card-header">
                <div>
                    <h3 class="card-title">${bot.name}</h3>
                    <div class="card-platform"><i class="ri-cpu-line"></i> ${bot.platform} ${bot.hostname ? `| ${bot.hostname}` : ''}</div>
                </div>
                <i class="${icon} bot-icon"></i>
            </div>
            
            <div style="margin-top: 1rem; margin-bottom: 1rem;">
                <span class="status-badge ${statusClass}">${statusText}</span>
            </div>
            
            <div class="card-details">
                ${extraDetails}
            </div>
            
            ${pingDetails}
            
            <div class="version-tag">
                <i class="ri-git-branch-line" style="font-size: 0.9em; opacity: 0.7;"></i> 
                ${bot.version || 'N/A'}
            </div>
        `;
        
        grid.appendChild(card);
    });
}

function updateStats(bots) {
    document.getElementById('total-bots').textContent = bots.length;
    
    const active = bots.filter(b => {
        if (b.type === 'shipped') return false;
        const s = (b.status || '').toLowerCase();
        return s.includes('active') || s.includes('online') || s.includes('ok');
    }).length;
    
    document.getElementById('active-bots').textContent = active;
}

function filterBots(e) {
    const term = e.target.value.toLowerCase();
    const cards = document.querySelectorAll('.bot-card');
    
    cards.forEach(card => {
        const name = card.dataset.name || '';
        const platform = card.dataset.platform || '';
        const customer = card.dataset.customer || '';
        const hostname = card.dataset.hostname || '';
        
        if (name.includes(term) || platform.includes(term) || customer.includes(term) || hostname.includes(term)) {
            card.style.display = 'block';
        } else {
            card.style.display = 'none';
        }
    });
}
