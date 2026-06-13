const navLinks = document.querySelectorAll('header a');

navLinks.forEach(link => {
    link.addEventListener('click', function(e) {
        e.preventDefault();

        const targetId = this.getAttribute('href');
        const targetSection = document.querySelector(targetId);

        targetSection.scrollIntoView({ behavior: 'smooth' });
    });
});

const sections = document.querySelectorAll('section');

window.addEventListener('scroll', function() {
    let currentSection = '';

    sections.forEach(section => {
        const sectionTop = section.offsetTop - 80;

        if (window.scrollY >= sectionTop) {
            currentSection = section.getAttribute('id');
        }
    });

    navLinks.forEach(link => {
        link.classList.remove('active');

        if (link.getAttribute('href') === '#' + currentSection) {
            link.classList.add('active');
        }
    });
});

const faders = document.querySelectorAll('.fade-in');

const observer = new IntersectionObserver(function(entries) {
    entries.forEach(entry => {
        if (entry.isIntersecting) {
            entry.target.classList.add('visible');
        }
    });
}, { threshold: 0.1 });


faders.forEach(fader => {
    observer.observe(fader);
});

const githubUsername = 'yourusername';

fetch(`https://api.github.com/users/${githubUsername}/repos?sort=updated&per_page=6`)
    .then(response => response.json())
    .then(repos => {
        const container = document.getElementById('github-projects');
        container.innerHTML = '';

        repos.forEach(repo => {
            const card = document.createElement('div');
            card.classList.add('github-card');

            card.innerHTML = `
                <h3>${repo.name}</h3>
                <p>${repo.description || 'No description provided.'}</p>
                <a href="${repo.html_url}" target="_blank">View on Github</a>
                `;

            container.appendChild(card);
        });
    })
    .catch(error => {
        document.getElementById('github-projects').innerHTML = '<p>Could not load projects.</p>';
        console.error(error);
    });